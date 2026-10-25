#!/usr/bin/env python3
# The network's structure is transcribed from AP-BWE, models/model.py (APNet_BWE_Model and ConvNeXtBlock) at
# yxlu-0102/AP-BWE 751710f, Copyright (c) 2023 Ye-Xin Lu, licensed under the MIT License (LICENSES/MIT-AP-BWE.txt);
# modified: rewritten as ONNX graph construction for Montage.
"""Generates src/media/apbwe.onnx: the AP-BWE speech bandwidth extension network (Ye-Xin Lu, Yang Ai, Hui-Peng Du and
Zhen-Hua Ling, 2024; code and weights MIT) as an ONNX graph without its weights. Each weight is external data naming
its bytes inside the authors' own generator checkpoint (g_<low>kto<high>k, a PyTorch zip whose tensors are stored
uncompressed), which Montage downloads and keeps as generator.ckpt, so the weights come only from their authors. All
seven checkpoints (2, 4 and 8 to 16 kHz; 8, 12, 16 and 24 to 48 kHz) hold the same tensors at the same offsets, so one
graph serves every one of them; pass each to check that:

  python3 scripts/gen-apbwe-graph.py src/media/apbwe.onnx <g_8kto16k> [<g_2kto16k> ...]

Needs onnx and numpy only. The network, on the STFT of the band-limited speech at the high rate (torch.stft with
n_fft 1024, a 320-sample Hann window every 80 samples, centred with reflection):

  input  "log_amp" [1, 513, T]: log(|X| + 1e-4)
  input  "pha"     [1, 513, T]: the phase of X
  output "log_amp_wb" [1, 513, T]: the wide band's log magnitude
  output "pha_r", "pha_i" [1, 513, T]: the wide band's phase as atan2(pha_i, pha_r) (taken outside the graph)
"""
import os
import pickle
import struct
import sys
import zipfile

import numpy as np
import onnx
from onnx import TensorProto, helper

BINS, CHANNELS, LAYERS = 513, 512, 8
LN_EPSILON = 1e-6  # the model's LayerNorms


# --- the checkpoint: a PyTorch zip; its pickle read without PyTorch (as scripts/gen-bandit-graph.py does) ---

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
        assert pid[0] == "storage", pid
        kind = pid[1] if isinstance(pid[1], str) else getattr(pid[1], "__name__", str(pid[1]))
        return {"type": kind, "key": str(pid[2]), "numel": pid[4]}


def read_checkpoint(path):
    """The generator's tensors, each as (byte offset in the file, shape)."""
    z = zipfile.ZipFile(path)
    pkl = next(n for n in z.namelist() if n.endswith("/data.pkl") or n == "data.pkl")
    prefix = pkl[: -len("data.pkl")]
    state = Reader(z.open(pkl)).load()["generator"]
    raw = open(path, "rb")
    storages = {}
    for info in z.infolist():
        if info.filename.startswith(prefix + "data/"):
            assert info.compress_type == zipfile.ZIP_STORED, info.filename
            raw.seek(info.header_offset)
            header = raw.read(30)
            assert header[:4] == b"PK\x03\x04", info.filename
            name_len, extra_len = struct.unpack_from("<HH", header, 26)
            storages[info.filename[len(prefix) + 5:]] = (info.header_offset + 30 + name_len + extra_len, info.file_size)
    tensors = {}
    for key, t in state.items():
        assert t.storage["type"] == "FloatStorage", key
        expect = tuple(int(np.prod(t.size[i + 1:])) for i in range(len(t.size)))
        assert t.stride == expect, (key, t.stride, expect)  # contiguous
        start, stored = storages[t.storage["key"]]
        numel = int(np.prod(t.size))
        assert (t.offset + numel) * 4 <= stored, key
        tensors[key] = (start + 4 * t.offset, t.size)
    return tensors


# --- the graph ---

class Graph:
    def __init__(self, tensors, location):
        self.tensors, self.location = tensors, location
        self.nodes, self.inits, self.n, self.used = [], [], 0, set()

    def op(self, kind, inputs, **attrs):
        self.n += 1
        out = f"{kind.lower()}_{self.n}"
        self.nodes.append(helper.make_node(kind, inputs, [out], **attrs))
        return out

    def const(self, value):
        self.n += 1
        name = f"const_{self.n}"
        self.inits.append(onnx.numpy_helper.from_array(np.array(value, dtype=np.float32), name))
        return name

    def weight(self, key, shape):
        """A tensor of the checkpoint, read in place."""
        offset, size = self.tensors[key]
        assert tuple(shape) == size, (key, shape, size)
        if key not in self.used:
            self.used.add(key)
            init = TensorProto()
            init.name, init.data_type = key, TensorProto.FLOAT
            init.dims.extend(size)
            init.data_location = TensorProto.EXTERNAL
            for k, v in (("location", self.location), ("offset", str(offset)), ("length", str(4 * int(np.prod(size))))):
                x = init.external_data.add()
                x.key, x.value = k, v
            self.inits.append(init)
        return key

    def transpose(self, x):  # [B, C, T] <-> [B, T, C]
        return self.op("Transpose", [x], perm=[0, 2, 1])

    def layer_norm(self, x, base):  # over the last axis
        return self.op("LayerNormalization", [x, self.weight(base + ".weight", (CHANNELS,)), self.weight(base + ".bias", (CHANNELS,))],
                       axis=-1, epsilon=LN_EPSILON)

    def linear(self, x, base, n_in, n_out):
        w = self.op("Transpose", [self.weight(base + ".weight", (n_out, n_in))], perm=[1, 0])
        return self.op("Add", [self.op("MatMul", [x, w]), self.weight(base + ".bias", (n_out,))])

    def conv(self, x, base, n_in, n_out, groups=1):  # kernel 7, padding 3
        return self.op("Conv", [x, self.weight(base + ".weight", (n_out, n_in // groups, 7)), self.weight(base + ".bias", (n_out,))],
                       kernel_shape=[7], pads=[3, 3], group=groups)

    def gelu(self, x):  # nn.GELU: x / 2 (1 + erf(x / sqrt 2))
        e = self.op("Erf", [self.op("Mul", [x, self.const(1 / np.sqrt(2))])])
        return self.op("Mul", [self.op("Mul", [x, self.const(0.5)]), self.op("Add", [e, self.const(1.0)])])

    def block(self, x, base):
        """ConvNeXtBlock on x [1, C, T]."""
        y = self.transpose(self.conv(x, base + ".dwconv", CHANNELS, CHANNELS, groups=CHANNELS))
        y = self.layer_norm(y, base + ".norm")
        y = self.linear(self.gelu(self.linear(y, base + ".pwconv1", CHANNELS, 3 * CHANNELS)), base + ".pwconv2", 3 * CHANNELS, CHANNELS)
        y = self.op("Mul", [self.weight(base + ".gamma", (CHANNELS,)), y])
        return self.op("Add", [x, self.transpose(y)])


def build(tensors, location):
    g = Graph(tensors, location)
    x_mag = g.transpose(g.layer_norm(g.transpose(g.conv("log_amp", "conv_pre_mag", BINS, CHANNELS)), "norm_pre_mag"))
    x_pha = g.transpose(g.layer_norm(g.transpose(g.conv("pha", "conv_pre_pha", BINS, CHANNELS)), "norm_pre_pha"))
    for i in range(LAYERS):
        x_mag = g.op("Add", [x_mag, x_pha])
        x_pha = g.op("Add", [x_pha, x_mag])
        x_mag = g.block(x_mag, f"convnext_mag.{i}")
        x_pha = g.block(x_pha, f"convnext_pha.{i}")
    x_mag = g.layer_norm(g.transpose(x_mag), "norm_post_mag")
    mag = g.op("Add", ["log_amp", g.transpose(g.linear(x_mag, "linear_post_mag", CHANNELS, BINS))])
    x_pha = g.layer_norm(g.transpose(x_pha), "norm_post_pha")
    pha_r = g.transpose(g.linear(x_pha, "linear_post_pha_r", CHANNELS, BINS))
    pha_i = g.transpose(g.linear(x_pha, "linear_post_pha_i", CHANNELS, BINS))
    g.nodes.append(helper.make_node("Identity", [mag], ["log_amp_wb"]))
    g.nodes.append(helper.make_node("Identity", [pha_r], ["pha_r"]))
    g.nodes.append(helper.make_node("Identity", [pha_i], ["pha_i"]))
    assert g.used == set(tensors), set(tensors) - g.used  # every tensor used
    spec = lambda name: helper.make_tensor_value_info(name, TensorProto.FLOAT, [1, BINS, "T"])
    graph = helper.make_graph(g.nodes, "ap-bwe", [spec("log_amp"), spec("pha")], [spec("log_amp_wb"), spec("pha_r"), spec("pha_i")],
                              g.inits)
    return helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)], producer_name="montage gen-apbwe-graph.py",
                             ir_version=8)  # (what ONNX Runtime 1.14 and later read)


def main():
    out, checkpoints = sys.argv[1], sys.argv[2:]
    tensors = read_checkpoint(checkpoints[0])
    for other in checkpoints[1:]:
        assert read_checkpoint(other) == tensors, other  # the same layout
    model = build(tensors, "generator.ckpt")
    # (checked beside the checkpoint, where its external data resolves)
    beside = os.path.join(os.path.dirname(os.path.abspath(checkpoints[0])), "generator.ckpt")
    check = os.path.join(os.path.dirname(beside), "gen-apbwe-check.onnx")
    linked = not os.path.exists(beside)
    if linked:
        os.symlink(os.path.abspath(checkpoints[0]), beside)
    try:
        onnx.save(model, check)
        onnx.checker.check_model(check)
    finally:
        os.remove(check)
        if linked:
            os.remove(beside)
    onnx.save(model, out)
    print(f"{out}: {len(model.graph.node)} nodes, {len(model.graph.initializer)} initializers")


if __name__ == "__main__":
    main()
