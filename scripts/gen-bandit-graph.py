#!/usr/bin/env python3
"""Generates src/media/bandit-v2.onnx: the Bandit v2 cinematic source separation network
(Watcharasupat, Wu and Lerch; code Apache-2.0, checkpoints CC BY-SA 4.0) as an ONNX graph without
its weights. Each weight is external data naming its bytes inside the authors' own checkpoint
(checkpoint-<language>.ckpt from https://zenodo.org/records/12701995, a PyTorch zip whose tensors
are stored uncompressed), which Montage downloads, so the weights come only from their authors.
Each checkpoint's tensors sit at their own offsets, so each language has its own graph:

  python3 scripts/gen-bandit-graph.py <checkpoint-eng.ckpt> src/media/bandit-v2-eng.onnx

Needs onnx and numpy only. The network is src/models/bandit (bandit.py, bandsplit.py, tfmodel.py,
maskestim.py, utils.py) at kwatcharasupat/bandit-v2 d5563d9 with configs/models/bandit-mus64.yaml
(48 kHz mono, 64 musical bands, 8 pairs of bidirectional GRUs over time and over bands, complex
masks), transcribed from its STFT on:

  input  "spectrogram" [N, T, 1025, 2]: the complex spectrogram of N mono chunks (torchaudio's,
                                         normalized), frames by bins, real and imaginary
  output "masks"       [N, 3, T, 1025, 2]: complex masks for speech, music and effects
"""
import os
import pickle
import struct
import sys
import zipfile

import numpy as np
import onnx
from onnx import TensorProto, helper

STEMS = ["speech", "music", "sfx"]
N_FFT, FS, N_BANDS, EMB, RNN, MLP, N_MODULES = 2048, 48000, 64, 128, 256, 512, 8
N_FREQ = N_FFT // 2 + 1
LN_EPSILON = 1e-5  # torch.nn.LayerNorm's default


# --- the checkpoint: a PyTorch zip; its pickle read without PyTorch ---

class Stub:
    def __init__(self, *a, **k):
        pass

    def __setstate__(self, state):
        pass

    def __call__(self, *a, **k):
        return Stub()


class Tensor:
    def __init__(self, storage, offset, size, stride):
        self.storage, self.offset, self.size, self.stride = storage, offset, tuple(size), tuple(stride)


def rebuild_tensor(storage, offset, size, stride, *rest):
    return Tensor(storage, offset, size, stride)


class Reader(pickle.Unpickler):
    def find_class(self, module, name):
        if module == "torch._utils" and name in ("_rebuild_tensor_v2", "_rebuild_tensor"):
            return rebuild_tensor
        if module == "collections" and name == "OrderedDict":
            import collections
            return collections.OrderedDict
        if module == "torch" and name.endswith("Storage"):
            return name
        if module.startswith("builtins"):
            import builtins
            return getattr(builtins, name)
        return Stub

    def persistent_load(self, pid):
        # ("storage", storage type, key, location, numel)
        assert pid[0] == "storage", pid
        kind = pid[1] if isinstance(pid[1], str) else getattr(pid[1], "__name__", str(pid[1]))
        return {"type": kind, "key": str(pid[2]), "numel": pid[4]}


def read_checkpoint(path):
    z = zipfile.ZipFile(path)
    pkl = next(n for n in z.namelist() if n.endswith("/data.pkl") or n == "data.pkl")
    prefix = pkl[: -len("data.pkl")]
    ckpt = Reader(z.open(pkl)).load()
    raw = open(path, "rb")
    offsets = {}
    for info in z.infolist():
        if info.filename.startswith(prefix + "data/"):
            assert info.compress_type == zipfile.ZIP_STORED, info.filename
            raw.seek(info.header_offset)
            header = raw.read(30)
            assert header[:4] == b"PK\x03\x04", info.filename
            name_len, extra_len = struct.unpack_from("<HH", header, 26)
            offsets[info.filename[len(prefix) + 5:]] = (info.header_offset + 30 + name_len + extra_len, info.file_size)
    return ckpt["state_dict"], offsets


# --- the bands (utils.py musical_filterbank, PerceptualBandsplitSpecification) ---

def hz_to_midi(f):
    return 12 * (np.log2(np.asanyarray(f)) - np.log2(440.0)) + 69


def midi_to_hz(m):
    return 440.0 * 2.0 ** ((np.asanyarray(m) - 69.0) / 12.0)


def musical_bands():
    nfft, n_freqs = N_FFT, N_FREQ
    df = FS / nfft
    f_max, f_min = FS / 2, FS / nfft
    bandwidth_mult = 2.0 ** (np.log2(f_max / f_min) / N_BANDS)
    hz = midi_to_hz(np.linspace(max(0, hz_to_midi(f_min)), hz_to_midi(f_max), N_BANDS))
    low_bins = np.floor(hz / bandwidth_mult / df).astype(int)
    high_bins = np.ceil(hz * bandwidth_mult / df).astype(int)
    fb = np.zeros((N_BANDS, n_freqs))
    for i in range(N_BANDS):
        fb[i, low_bins[i]: high_bins[i] + 1] = 1.0
    fb[0, : low_bins[0]] = 1.0
    fb[-1, high_bins[-1] + 1:] = 1.0
    weights = fb / fb.sum(axis=0, keepdims=True)
    bands = []
    for i in range(N_BANDS):
        active = np.nonzero(fb[i])[0]
        bands.append((int(active[0]), int(active[-1]) + 1, weights[i, active[0]: active[-1] + 1]))
    return bands


# --- the graph ---

class Graph:
    def __init__(self, state, offsets, location):
        self.state, self.offsets, self.location = state, offsets, location
        self.nodes, self.inits, self.n, self.used = [], [], 0, {}

    def name(self, prefix):
        self.n += 1
        return f"{prefix}_{self.n}"

    def op(self, kind, inputs, outputs=1, **attrs):
        outs = [self.name(kind.lower()) for _ in range(outputs)]
        self.nodes.append(helper.make_node(kind, inputs, outs, **attrs))
        return outs[0] if outputs == 1 else outs

    def const(self, values, dtype=np.int64):
        name = self.name("const")
        self.inits.append(onnx.numpy_helper.from_array(np.array(values, dtype=dtype), name))
        return name

    def weight(self, key, shape=None):
        """A tensor of the checkpoint, read in place (as float: the band weights are stored as double)."""
        if key in self.used:
            return self.used[key]
        t = self.state[key]
        kind, size = {"FloatStorage": (TensorProto.FLOAT, 4), "DoubleStorage": (TensorProto.DOUBLE, 8)}[t.storage["type"]]
        expect = tuple(int(np.prod(t.size[i + 1:])) for i in range(len(t.size)))
        assert t.stride == expect, (key, t.stride, expect)  # contiguous
        start, stored = self.offsets[t.storage["key"]]
        numel = int(np.prod(t.size))
        assert (t.offset + numel) * size <= stored, key
        if shape is not None:
            assert tuple(shape) == t.size, (key, shape, t.size)
        init = TensorProto()
        init.name, init.data_type = key, kind
        init.dims.extend(t.size)
        init.data_location = TensorProto.EXTERNAL
        for k, v in (("location", self.location), ("offset", str(start + size * t.offset)), ("length", str(size * numel))):
            x = init.external_data.add()
            x.key, x.value = k, v
        self.inits.append(init)
        self.used[key] = key if kind == TensorProto.FLOAT else self.op("Cast", [key], to=TensorProto.FLOAT)
        return self.used[key]

    def layer_norm(self, x, base, width):
        return self.op("LayerNormalization", [x, self.weight(base + ".weight", (width,)), self.weight(base + ".bias", (width,))],
                       axis=-1, epsilon=LN_EPSILON)

    def linear(self, x, base, n_in, n_out):
        w = self.op("Transpose", [self.weight(base + ".weight", (n_out, n_in))], perm=[1, 0])
        return self.op("Add", [self.op("MatMul", [x, w]), self.weight(base + ".bias", (n_out,))])


def gru_weights(g, base, suffix):
    """PyTorch's gates (r, z, n) in ONNX's order (z, r, h)."""
    def reorder(name, shape):
        r, z, n = g.op("Split", [g.weight(name, shape), g.const([RNN, RNN, RNN])], outputs=3, axis=0)
        return g.op("Concat", [z, r, n], axis=0)

    w = reorder(f"{base}.weight_ih_l0{suffix}", (3 * RNN, EMB))
    r = reorder(f"{base}.weight_hh_l0{suffix}", (3 * RNN, RNN))
    b = g.op("Concat", [reorder(f"{base}.bias_ih_l0{suffix}", (3 * RNN,)), reorder(f"{base}.bias_hh_l0{suffix}", (3 * RNN,))], axis=0)
    return w, r, b


def residual_rnn(g, z, base, across):
    """tfmodel.py ResidualRNN on z [N, A, B, E], the GRU running along B (of length `across`)."""
    y = g.layer_norm(z, base + ".norm", EMB)
    y = g.op("Reshape", [y, g.const([-1, across, EMB])])                # [N*A, B, E]
    y = g.op("Transpose", [y], perm=[1, 0, 2])                           # [B, N*A, E]
    fw, bw = gru_weights(g, base + ".rnn", ""), gru_weights(g, base + ".rnn", "_reverse")
    W = g.op("Concat", [g.op("Unsqueeze", [fw[0], g.const([0])]), g.op("Unsqueeze", [bw[0], g.const([0])])], axis=0)
    R = g.op("Concat", [g.op("Unsqueeze", [fw[1], g.const([0])]), g.op("Unsqueeze", [bw[1], g.const([0])])], axis=0)
    B = g.op("Concat", [g.op("Unsqueeze", [fw[2], g.const([0])]), g.op("Unsqueeze", [bw[2], g.const([0])])], axis=0)
    y = g.op("GRU", [y, W, R, B], hidden_size=RNN, direction="bidirectional", linear_before_reset=1)  # [B, 2, N*A, H]
    y = g.op("Transpose", [y], perm=[2, 0, 1, 3])                        # [N*A, B, 2, H]
    shape = g.op("Shape", [z])
    y = g.op("Reshape", [y, g.op("Concat", [g.op("Slice", [shape, g.const([0]), g.const([3])]), g.const([2 * RNN])], axis=0)])
    y = g.linear(y, base + ".fc", 2 * RNN, EMB)
    return g.op("Add", [y, z])


def build(state, offsets, location, frames):
    g = Graph(state, offsets, location)
    bands = musical_bands()

    # The band split: each band's bins (real, imaginary) normalised and projected to EMB.
    zs = []
    for i, (fs, fe, _) in enumerate(bands):
        bw = fe - fs
        xb = g.op("Slice", ["spectrogram", g.const([fs]), g.const([fe]), g.const([2])])
        xb = g.op("Reshape", [xb, g.const([-1, frames, 2 * bw])])
        base = f"model.band_split.norm_fc_modules.{i}.combined"
        xb = g.layer_norm(xb, base + ".0", 2 * bw)
        xb = g.linear(xb, base + ".1", 2 * bw, EMB)
        zs.append(g.op("Unsqueeze", [xb, g.const([1])]))
    z = g.op("Concat", zs, axis=1)                                       # [N, bands, T, E]

    # Time and band modelling: GRUs over time, then over bands, N_MODULES times each.
    tf = "model.tf_model.seqband"
    for k in range(2 * N_MODULES):
        z = residual_rnn(g, z, f"{tf}.{2 * k}", frames if k % 2 == 0 else N_BANDS)
        z = g.op("Transpose", [z], perm=[0, 2, 1, 3])
    # (an even number of transposes: z is [N, bands, T, E] again)

    # Mask estimation: per stem, per band an MLP gives a complex mask, weighted where bands overlap and summed.
    masks = []
    for stem in STEMS:
        total = None
        for i, (fs, fe, weights) in enumerate(bands):
            bw = fe - fs
            qb = g.op("Gather", [z, g.const(i)], axis=1)                 # [N, T, E]
            base = f"model.mask_estim.{stem}.norm_mlp.{i}"
            m = g.layer_norm(qb, base + ".norm", EMB)
            m = g.op("Tanh", [g.linear(m, base + ".hidden.0", EMB, MLP)])
            m = g.linear(m, base + ".output.0", MLP, 4 * bw)
            a, gate = g.op("Split", [m, g.const([2 * bw, 2 * bw])], outputs=2, axis=-1)
            m = g.op("Mul", [a, g.op("Sigmoid", [gate])])                  # GLU
            m = g.op("Reshape", [m, g.const([-1, frames, bw, 2])])
            fw = g.weight(f"model.mask_estim.{stem}.freq_weights/{i}", (bw,))
            m = g.op("Mul", [m, g.op("Reshape", [fw, g.const([1, 1, bw, 1])])])
            m = g.op("Pad", [m, g.const([0, 0, fs, 0, 0, 0, N_FREQ - fe, 0])])
            total = m if total is None else g.op("Add", [total, m])
        masks.append(g.op("Unsqueeze", [total, g.const([1])]))
    out = g.op("Concat", masks, axis=1)
    g.nodes.append(helper.make_node("Identity", [out], ["masks"]))

    graph = helper.make_graph(
        g.nodes, "bandit-v2",
        [helper.make_tensor_value_info("spectrogram", TensorProto.FLOAT, ["N", frames, N_FREQ, 2])],
        [helper.make_tensor_value_info("masks", TensorProto.FLOAT, ["N", len(STEMS), frames, N_FREQ, 2])],
        g.inits)
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)], producer_name="montage gen-bandit-graph.py",
                              ir_version=8)  # (what ONNX Runtime 1.14 and later read)
    return model, bands


def check_bands(path, state, offsets, bands):
    """The frequency weights stored in the checkpoint are the ones these bands give."""
    raw = open(path, "rb")
    for stem in STEMS:
        for i, (fs, fe, weights) in enumerate(bands):
            t = state[f"model.mask_estim.{stem}.freq_weights/{i}"]
            start, _ = offsets[t.storage["key"]]
            assert t.storage["type"] == "DoubleStorage"
            raw.seek(start + 8 * t.offset)
            stored = np.frombuffer(raw.read(8 * (fe - fs)), dtype="<f8")
            assert t.size == (fe - fs,) and np.allclose(stored, weights, atol=1e-6), (stem, i)


def main():
    ckpt, out = sys.argv[1], sys.argv[2]
    frames = int(sys.argv[3]) if len(sys.argv) > 3 else 751  # 8 s at 48 kHz, every 512 samples, centred
    state, offsets = read_checkpoint(ckpt)
    model, bands = build(state, offsets, os.path.basename(ckpt), frames)
    check_bands(ckpt, state, offsets, bands)
    # (checked beside the checkpoint, where its external data resolves)
    beside = os.path.join(os.path.dirname(os.path.abspath(ckpt)), "gen-bandit-check.onnx")
    onnx.save(model, beside)
    try:
        onnx.checker.check_model(beside)
    finally:
        os.remove(beside)
    onnx.save(model, out)
    print(f"{out}: {len(model.graph.node)} nodes, {len(model.graph.initializer)} initializers")


if __name__ == "__main__":
    main()
