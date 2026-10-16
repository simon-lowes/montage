#!/usr/bin/env python3
# The networks' structure is transcribed from MuseTalk 1.5 (Tencent Music Entertainment; MIT: musetalk/models/unet.py,
# musetalk/utils/audio_processor.py and musetalk/models/vae.py at TMElyralab/MuseTalk), which runs diffusers'
# UNet2DConditionModel and AutoencoderKL (Apache-2.0) and the encoder of OpenAI's Whisper through transformers'
# WhisperModel (Apache-2.0; the feature extractor's log-mel spectrogram included); rewritten as ONNX graph construction
# for Montage.
"""Generates the lip-sync networks (media/LipSync) as ONNX graphs without their weights. Each weight is external data
naming its bytes inside its authors' own file, which Montage downloads, so the weights come only from them:

  src/media/lipsync-unet.onnx         MuseTalk 1.5's UNet (musetalkV15/unet.pth at TMElyralab/MuseTalk 2bcb936,
                                      MIT; a PyTorch zip whose tensors are stored uncompressed)
  src/media/lipsync-vae-encoder.onnx  sd-vae-ft-mse's encoder and decoder (diffusion_pytorch_model.safetensors at
  src/media/lipsync-vae-decoder.onnx  stabilityai/sd-vae-ft-mse 31f26fd, MIT)
  src/media/lipsync-audio.onnx        whisper-tiny's encoder, from 30 s of 16 kHz sound (model.safetensors at
                                      openai/whisper-tiny 169d4a4, Apache-2.0)

  python3 scripts/gen-lipsync-graphs.py <dir holding unet.pth, sd-vae-ft-mse.safetensors and whisper-tiny.safetensors> src/media

Needs onnx and numpy only. The graphs:

  unet         "latents" [N, 8, 32, 32]: the masked face's latents and the reference face's; "audio" [N, 50, 384]: ten
               whisper frames (every 20 ms around the video frame) of five layers each -> "out" [N, 4, 32, 32]: the face's
               latents (the network is run once, at timestep 0)
  vae-encoder  "image" [N, 3, 256, 256] from -1 to 1 -> "latents" [N, 4, 32, 32]: the mean, scaled by 0.18215
  vae-decoder  "latents" [N, 4, 32, 32] -> "image" [N, 3, 256, 256] from 0 to 1
  audio        "pcm" [1, 480000]: 30 s at 16 kHz -> "features" [1500, 5, 384]: the encoder's input and each layer's
               output (the last normalised), every 20 ms
"""
import json
import math
import os
import pickle
import struct
import sys
import zipfile

import numpy as np
import onnx
from onnx import TensorProto, helper

# --- the weights: a PyTorch zip read without PyTorch, or safetensors ---


class Stub:
    def __init__(self, *a, **k):
        pass

    def __setstate__(self, state):
        pass

    def __call__(self, *a, **k):
        return Stub()


class Tensor:
    def __init__(self, start, size, kind):
        self.start, self.size, self.kind = start, tuple(size), kind  # (absolute byte offset in the file)


class Reader(pickle.Unpickler):
    def find_class(self, module, name):
        if module == "torch._utils" and name in ("_rebuild_tensor_v2", "_rebuild_tensor"):
            return lambda storage, offset, size, stride, *rest: (storage, offset, tuple(size), tuple(stride))
        if module == "collections" and name == "OrderedDict":
            import collections
            return collections.OrderedDict
        if module == "torch" and name.endswith("Storage"):
            return name
        return Stub

    def persistent_load(self, pid):
        assert pid[0] == "storage", pid
        kind = pid[1] if isinstance(pid[1], str) else getattr(pid[1], "__name__", str(pid[1]))
        return {"type": kind, "key": str(pid[2])}


def read_torch(path):
    z = zipfile.ZipFile(path)
    pkl = next(n for n in z.namelist() if n.endswith("/data.pkl") or n == "data.pkl")
    prefix = pkl[: -len("data.pkl")]
    state = Reader(z.open(pkl)).load()
    raw = open(path, "rb")
    starts = {}
    for info in z.infolist():
        if info.filename.startswith(prefix + "data/"):
            assert info.compress_type == zipfile.ZIP_STORED, info.filename
            raw.seek(info.header_offset)
            header = raw.read(30)
            assert header[:4] == b"PK\x03\x04", info.filename
            name_len, extra_len = struct.unpack_from("<HH", header, 26)
            starts[info.filename[len(prefix) + 5:]] = info.header_offset + 30 + name_len + extra_len
    out = {}
    for key, (storage, offset, size, stride) in state.items():
        assert storage["type"] == "FloatStorage", key
        expect = tuple(int(np.prod(size[i + 1:])) for i in range(len(size)))
        assert stride == expect or int(np.prod(size)) <= 1, (key, stride, expect)  # contiguous
        out[key] = Tensor(starts[storage["key"]] + 4 * offset, size, TensorProto.FLOAT)
    return out


def read_safetensors(path):
    raw = open(path, "rb")
    n = struct.unpack("<Q", raw.read(8))[0]
    header = json.loads(raw.read(n))
    out = {}
    for key, t in header.items():
        if key == "__metadata__":
            continue
        assert t["dtype"] == "F32", key
        begin, end = t["data_offsets"]
        assert end - begin == 4 * int(np.prod(t["shape"])), key
        out[key] = Tensor(8 + n + begin, t["shape"], TensorProto.FLOAT)
    return out


# --- the graph ---


class Graph:
    def __init__(self, weights, location):
        self.weights, self.location = weights, location
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

    def fconst(self, values):
        return self.const(values, np.float32)

    def has(self, key):
        return key in self.weights

    def shape(self, key):
        return self.weights[key].size

    def weight(self, key):
        """A tensor of the weights' file, read in place."""
        if key in self.used:
            return self.used[key]
        t = self.weights[key]
        init = TensorProto()
        init.name, init.data_type = key, t.kind
        init.dims.extend(t.size)
        init.data_location = TensorProto.EXTERNAL
        for k, v in (("location", self.location), ("offset", str(t.start)), ("length", str(4 * int(np.prod(t.size))))):
            x = init.external_data.add()
            x.key, x.value = k, v
        self.inits.append(init)
        self.used[key] = key
        return key

    # Layers, as PyTorch's.

    def linear(self, x, base, bias=True):
        w = self.op("Transpose", [self.weight(base + ".weight")], perm=[1, 0])
        y = self.op("MatMul", [x, w])
        return self.op("Add", [y, self.weight(base + ".bias")]) if bias else y

    def conv(self, x, base, stride=1, pad=None):
        k = self.shape(base + ".weight")
        p = (k[-1] // 2) if pad is None else pad
        pads = [p] * (2 * (len(k) - 2)) if isinstance(p, int) else p
        inputs = [x, self.weight(base + ".weight")] + ([self.weight(base + ".bias")] if self.has(base + ".bias") else [])
        return self.op("Conv", inputs, strides=[stride] * (len(k) - 2), pads=pads)

    def layer_norm(self, x, base, eps=1e-5):
        return self.op("LayerNormalization", [x, self.weight(base + ".weight"), self.weight(base + ".bias")], axis=-1, epsilon=eps)

    def group_norm(self, x, base, groups=32, eps=1e-5):
        """GroupNorm on [N, C, H, W]: instance normalisation over the groups, then each channel's scale and shift."""
        c = self.shape(base + ".weight")[0]
        y = self.op("Reshape", [x, self.const([0, groups, -1])])
        y = self.op("InstanceNormalization", [y, self.fconst(np.ones(groups)), self.fconst(np.zeros(groups))], epsilon=eps)
        y = self.op("Reshape", [y, self.op("Shape", [x])])
        y = self.op("Mul", [y, self.op("Reshape", [self.weight(base + ".weight"), self.const([c, 1, 1])])])
        return self.op("Add", [y, self.op("Reshape", [self.weight(base + ".bias"), self.const([c, 1, 1])])])

    def silu(self, x):
        return self.op("Mul", [x, self.op("Sigmoid", [x])])

    def gelu(self, x):  # (exact, as PyTorch's)
        e = self.op("Erf", [self.op("Mul", [x, self.fconst(1 / math.sqrt(2))])])
        return self.op("Mul", [self.op("Mul", [x, self.fconst(0.5)]), self.op("Add", [e, self.fconst(1.0)])])

    def attention(self, q, k, v, heads):
        """Scaled dot-product attention on [N, L, C] (several heads)."""
        def split(t):
            return self.op("Transpose", [self.op("Reshape", [t, self.const([0, 0, heads, -1])])], perm=[0, 2, 1, 3])
        q, k, v = split(q), split(k), split(v)
        d = self.op("Cast", [self.op("Slice", [self.op("Shape", [q]), self.const([3]), self.const([4])])], to=TensorProto.FLOAT)
        scale = self.op("Reciprocal", [self.op("Sqrt", [d])])
        s = self.op("Mul", [self.op("MatMul", [q, self.op("Transpose", [k], perm=[0, 1, 3, 2])]), scale])
        y = self.op("MatMul", [self.op("Softmax", [s], axis=-1), v])
        return self.op("Reshape", [self.op("Transpose", [y], perm=[0, 2, 1, 3]), self.const([0, 0, -1])])

    def output(self, x, name):
        self.nodes.append(helper.make_node("Identity", [x], [name]))

    def model(self, name, inputs, outputs):
        graph = helper.make_graph(self.nodes, name, inputs, outputs, self.inits)
        return helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)], producer_name="montage gen-lipsync-graphs.py",
                                 ir_version=8)  # (what ONNX Runtime 1.14 and later read)


def tokens(g, x):
    """[N, C, H, W] -> [N, H*W, C]."""
    return g.op("Transpose", [g.op("Reshape", [x, g.const([0, 0, -1])])], perm=[0, 2, 1])


def untokens(g, t, like):
    """[N, H*W, C] -> the shape of `like` ([N, C, H, W])."""
    return g.op("Reshape", [g.op("Transpose", [t], perm=[0, 2, 1]), g.op("Shape", [like])])


# --- diffusers' blocks ---


def resnet(g, x, base, temb=None, eps=1e-5):
    """ResnetBlock2D: two norm-SiLU-convs (the time added between), with a shortcut."""
    h = g.conv(g.silu(g.group_norm(x, base + ".norm1", eps=eps)), base + ".conv1")
    if temb is not None:
        t = g.linear(g.silu(temb), base + ".time_emb_proj")
        h = g.op("Add", [h, g.op("Unsqueeze", [t, g.const([2, 3])])])
    h = g.conv(g.silu(g.group_norm(h, base + ".norm2", eps=eps)), base + ".conv2")
    if g.has(base + ".conv_shortcut.weight"):
        x = g.conv(x, base + ".conv_shortcut")
    return g.op("Add", [x, h])


def transformer(g, x, base, context, heads=8):
    """Transformer2DModel (one BasicTransformerBlock): self-attention, attention to the sound, GEGLU feed-forward."""
    h = g.group_norm(x, base + ".norm", eps=1e-6)
    if len(g.shape(base + ".proj_in.weight")) == 4:
        h = tokens(g, g.conv(h, base + ".proj_in"))
    else:
        h = g.linear(tokens(g, h), base + ".proj_in")
    b = base + ".transformer_blocks.0"
    n = g.layer_norm(h, b + ".norm1")
    a = g.attention(g.linear(n, b + ".attn1.to_q", False), g.linear(n, b + ".attn1.to_k", False), g.linear(n, b + ".attn1.to_v", False), heads)
    h = g.op("Add", [g.linear(a, b + ".attn1.to_out.0"), h])
    n = g.layer_norm(h, b + ".norm2")
    a = g.attention(g.linear(n, b + ".attn2.to_q", False), g.linear(context, b + ".attn2.to_k", False),
                    g.linear(context, b + ".attn2.to_v", False), heads)
    h = g.op("Add", [g.linear(a, b + ".attn2.to_out.0"), h])
    n = g.layer_norm(h, b + ".norm3")
    p = g.linear(n, b + ".ff.net.0.proj")
    half = g.shape(b + ".ff.net.0.proj.weight")[0] // 2
    value, gate = g.op("Split", [p, g.const([half, half])], outputs=2, axis=-1)
    h = g.op("Add", [g.linear(g.op("Mul", [value, g.gelu(gate)]), b + ".ff.net.2"), h])
    if len(g.shape(base + ".proj_out.weight")) == 4:
        h = g.conv(untokens(g, h, x), base + ".proj_out")
    else:
        h = untokens(g, g.linear(h, base + ".proj_out"), x)
    return g.op("Add", [h, x])


def upsample(g, x, base):
    y = g.op("Resize", [x, "", g.fconst([1, 1, 2, 2])], mode="nearest", coordinate_transformation_mode="asymmetric", nearest_mode="floor")
    return g.conv(y, base + ".conv")


def vae_attention(g, x, base):
    """The VAE's single-head attention over the picture's positions."""
    h = tokens(g, g.group_norm(x, base + ".group_norm", eps=1e-6))
    a = g.attention(g.linear(h, base + ".query"), g.linear(h, base + ".key"), g.linear(h, base + ".value"), 1)
    return g.op("Add", [untokens(g, g.linear(a, base + ".proj_attn"), x), x])


# --- the networks ---

SCALE = 0.18215  # sd-vae-ft-mse's latent scaling


def build_unet(weights, location):
    g = Graph(weights, location)
    # The sound's tokens with MuseTalk's sinusoidal positions (PositionalEncoding, d_model 384).
    pos = np.arange(50, dtype=np.float32)[:, None]
    div = np.exp(np.arange(0, 384, 2, dtype=np.float32) * (-math.log(10000.0) / 384))
    pe = np.zeros((1, 50, 384), dtype=np.float32)
    pe[0, :, 0::2], pe[0, :, 1::2] = np.sin(pos * div), np.cos(pos * div)
    context = g.op("Add", ["audio", g.fconst(pe)])
    # Timestep 0: its sinusoidal embedding (cosines first) is ones then zeros.
    t = g.fconst(np.concatenate([np.ones(160), np.zeros(160)])[None, :])
    temb = g.linear(g.silu(g.linear(t, "time_embedding.linear_1")), "time_embedding.linear_2")

    x = g.conv("latents", "conv_in")
    skips = [x]
    for i in range(4):
        for j in range(2):
            x = resnet(g, x, f"down_blocks.{i}.resnets.{j}", temb)
            if g.has(f"down_blocks.{i}.attentions.{j}.norm.weight"):
                x = transformer(g, x, f"down_blocks.{i}.attentions.{j}", context)
            skips.append(x)
        if g.has(f"down_blocks.{i}.downsamplers.0.conv.weight"):
            x = g.conv(x, f"down_blocks.{i}.downsamplers.0.conv", stride=2)
            skips.append(x)
    x = resnet(g, x, "mid_block.resnets.0", temb)
    x = transformer(g, x, "mid_block.attentions.0", context)
    x = resnet(g, x, "mid_block.resnets.1", temb)
    for i in range(4):
        for j in range(3):
            x = g.op("Concat", [x, skips.pop()], axis=1)
            x = resnet(g, x, f"up_blocks.{i}.resnets.{j}", temb)
            if g.has(f"up_blocks.{i}.attentions.{j}.norm.weight"):
                x = transformer(g, x, f"up_blocks.{i}.attentions.{j}", context)
        if g.has(f"up_blocks.{i}.upsamplers.0.conv.weight"):
            x = upsample(g, x, f"up_blocks.{i}.upsamplers.0")
    assert not skips
    x = g.conv(g.silu(g.group_norm(x, "conv_norm_out")), "conv_out")
    g.output(x, "out")
    return g.model("musetalk-unet", [helper.make_tensor_value_info("latents", TensorProto.FLOAT, ["N", 8, 32, 32]),
                                     helper.make_tensor_value_info("audio", TensorProto.FLOAT, ["N", 50, 384])],
                   [helper.make_tensor_value_info("out", TensorProto.FLOAT, ["N", 4, 32, 32])])


def build_vae_encoder(weights, location):
    g = Graph(weights, location)
    x = g.conv("image", "encoder.conv_in")
    for i in range(4):
        for j in range(2):
            x = resnet(g, x, f"encoder.down_blocks.{i}.resnets.{j}", eps=1e-6)
        if g.has(f"encoder.down_blocks.{i}.downsamplers.0.conv.weight"):
            x = g.op("Pad", [x, g.const([0, 0, 0, 0, 0, 0, 1, 1])])  # (right and bottom only)
            x = g.conv(x, f"encoder.down_blocks.{i}.downsamplers.0.conv", stride=2, pad=0)
    x = resnet(g, x, "encoder.mid_block.resnets.0", eps=1e-6)
    x = vae_attention(g, x, "encoder.mid_block.attentions.0")
    x = resnet(g, x, "encoder.mid_block.resnets.1", eps=1e-6)
    x = g.conv(g.silu(g.group_norm(x, "encoder.conv_norm_out", eps=1e-6)), "encoder.conv_out")
    x = g.conv(x, "quant_conv")
    mean = g.op("Slice", [x, g.const([0]), g.const([4]), g.const([1])])
    g.output(g.op("Mul", [mean, g.fconst(SCALE)]), "latents")
    return g.model("sd-vae-encoder", [helper.make_tensor_value_info("image", TensorProto.FLOAT, ["N", 3, 256, 256])],
                   [helper.make_tensor_value_info("latents", TensorProto.FLOAT, ["N", 4, 32, 32])])


def build_vae_decoder(weights, location):
    g = Graph(weights, location)
    x = g.conv(g.op("Mul", ["latents", g.fconst(1 / SCALE)]), "post_quant_conv")
    x = g.conv(x, "decoder.conv_in")
    x = resnet(g, x, "decoder.mid_block.resnets.0", eps=1e-6)
    x = vae_attention(g, x, "decoder.mid_block.attentions.0")
    x = resnet(g, x, "decoder.mid_block.resnets.1", eps=1e-6)
    for i in range(4):
        for j in range(3):
            x = resnet(g, x, f"decoder.up_blocks.{i}.resnets.{j}", eps=1e-6)
        if g.has(f"decoder.up_blocks.{i}.upsamplers.0.conv.weight"):
            x = upsample(g, x, f"decoder.up_blocks.{i}.upsamplers.0")
    x = g.conv(g.silu(g.group_norm(x, "decoder.conv_norm_out", eps=1e-6)), "decoder.conv_out")
    x = g.op("Clip", [g.op("Add", [g.op("Mul", [x, g.fconst(0.5)]), g.fconst(0.5)]), g.fconst(0.0), g.fconst(1.0)])
    g.output(x, "image")
    return g.model("sd-vae-decoder", [helper.make_tensor_value_info("latents", TensorProto.FLOAT, ["N", 4, 32, 32])],
                   [helper.make_tensor_value_info("image", TensorProto.FLOAT, ["N", 3, 256, 256])])


# Whisper's front end: 25 ms Hann windows every 10 ms, 80 mel bands (Slaney's), as its feature extractor computes them.
SR, N_FFT, HOP, N_MELS, SAMPLES = 16000, 400, 160, 80, 480000


def mel_filters():
    """librosa.filters.mel(sr=16000, n_fft=400, n_mels=80): Slaney's mel scale and area normalisation."""
    def hz_to_mel(f):
        f = np.asanyarray(f, dtype=np.float64)
        mel = f / (200.0 / 3)
        log = f >= 1000.0
        return np.where(log, 15.0 + np.log(np.maximum(f, 1e-10) / 1000.0) / (np.log(6.4) / 27.0), mel)

    def mel_to_hz(m):
        m = np.asanyarray(m, dtype=np.float64)
        f = m * (200.0 / 3)
        log = m >= 15.0
        return np.where(log, 1000.0 * np.exp((np.log(6.4) / 27.0) * (m - 15.0)), f)

    fft_freqs = np.linspace(0, SR / 2, 1 + N_FFT // 2)
    mel_f = mel_to_hz(np.linspace(hz_to_mel(0.0), hz_to_mel(SR / 2), N_MELS + 2))
    fdiff = np.diff(mel_f)
    ramps = mel_f[:, None] - fft_freqs[None, :]
    weights = np.zeros((N_MELS, len(fft_freqs)))
    for i in range(N_MELS):
        lower, upper = -ramps[i] / fdiff[i], ramps[i + 2] / fdiff[i + 1]
        weights[i] = np.maximum(0, np.minimum(lower, upper))
    weights *= (2.0 / (mel_f[2: N_MELS + 2] - mel_f[:N_MELS]))[:, None]
    return weights.astype(np.float32)


def build_audio(weights, location):
    g = Graph(weights, location)
    # The spectrogram: reflected by half a window at each end, then a convolution with the windowed DFT's real and
    # imaginary parts (a periodic Hann window), the power of each of its 3001 frames but the last.
    x = g.op("Unsqueeze", ["pcm", g.const([1])])                                  # [1, 1, S]
    x = g.op("Pad", [x, g.const([0, 0, N_FFT // 2, 0, 0, N_FFT // 2])], mode="reflect")
    n = np.arange(N_FFT)
    window = 0.5 - 0.5 * np.cos(2 * np.pi * n / N_FFT)
    k = np.arange(N_FFT // 2 + 1)[:, None]
    basis = np.concatenate([np.cos(2 * np.pi * k * n / N_FFT), -np.sin(2 * np.pi * k * n / N_FFT)]) * window
    spec = g.op("Conv", [x, g.fconst(basis[:, None, :])], strides=[HOP])          # [1, 402, 3001]
    re, im = g.op("Split", [spec, g.const([N_FFT // 2 + 1, N_FFT // 2 + 1])], outputs=2, axis=1)
    power = g.op("Add", [g.op("Mul", [re, re]), g.op("Mul", [im, im])])
    power = g.op("Slice", [power, g.const([0]), g.const([SAMPLES // HOP]), g.const([2])])
    mel = g.op("MatMul", [g.fconst(mel_filters()), power])                       # [1, 80, 3000]
    log = g.op("Div", [g.op("Log", [g.op("Max", [mel, g.fconst(1e-10)])]), g.fconst(math.log(10))])
    peak = g.op("ReduceMax", [log], keepdims=1)
    log = g.op("Max", [log, g.op("Sub", [peak, g.fconst(8.0)])])
    feats = g.op("Div", [g.op("Add", [log, g.fconst(4.0)]), g.fconst(4.0)])
    # The encoder: two convolutions (the second halving the rate to 50 a second), positions, four layers.
    e = "model.encoder"
    h = g.gelu(g.conv(feats, e + ".conv1"))
    h = g.gelu(g.conv(h, e + ".conv2", stride=2))
    h = g.op("Transpose", [h], perm=[0, 2, 1])                                   # [1, 1500, 384]
    h = g.op("Add", [h, g.weight(e + ".embed_positions.weight")])
    states = [h]
    for i in range(4):
        b = f"{e}.layers.{i}"
        n_ = g.layer_norm(h, b + ".self_attn_layer_norm")
        a = g.attention(g.linear(n_, b + ".self_attn.q_proj"), g.linear(n_, b + ".self_attn.k_proj", False),
                        g.linear(n_, b + ".self_attn.v_proj"), 6)
        h = g.op("Add", [h, g.linear(a, b + ".self_attn.out_proj")])
        n_ = g.layer_norm(h, b + ".final_layer_norm")
        h = g.op("Add", [h, g.linear(g.gelu(g.linear(n_, b + ".fc1")), b + ".fc2")])
        states.append(h)
    states[-1] = g.layer_norm(h, e + ".layer_norm")
    out = g.op("Concat", [g.op("Unsqueeze", [s, g.const([2])]) for s in states], axis=2)  # [1, 1500, 5, 384]
    g.output(g.op("Squeeze", [out, g.const([0])]), "features")
    return g.model("whisper-tiny-encoder", [helper.make_tensor_value_info("pcm", TensorProto.FLOAT, [1, SAMPLES])],
                   [helper.make_tensor_value_info("features", TensorProto.FLOAT, [1500, 5, 384])])


def save(model, weights_path, out):
    # (checked beside the weights, where their external data resolves)
    beside = os.path.join(os.path.dirname(os.path.abspath(weights_path)), "gen-lipsync-check.onnx")
    onnx.save(model, beside)
    try:
        onnx.checker.check_model(beside)
    finally:
        os.remove(beside)
    onnx.save(model, out)
    print(f"{out}: {len(model.graph.node)} nodes, {len(model.graph.initializer)} initializers")


def main():
    src, out = sys.argv[1], sys.argv[2]
    unet, vae, whisper = (os.path.join(src, f) for f in ("unet.pth", "sd-vae-ft-mse.safetensors", "whisper-tiny.safetensors"))
    w = read_torch(unet)
    save(build_unet(w, "unet.pth"), unet, os.path.join(out, "lipsync-unet.onnx"))
    w = read_safetensors(vae)
    save(build_vae_encoder(w, "sd-vae-ft-mse.safetensors"), vae, os.path.join(out, "lipsync-vae-encoder.onnx"))
    save(build_vae_decoder(w, "sd-vae-ft-mse.safetensors"), vae, os.path.join(out, "lipsync-vae-decoder.onnx"))
    w = read_safetensors(whisper)
    save(build_audio(w, "whisper-tiny.safetensors"), whisper, os.path.join(out, "lipsync-audio.onnx"))


if __name__ == "__main__":
    main()
