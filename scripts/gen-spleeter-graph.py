#!/usr/bin/env python3
"""Generates src/media/spleeter-<n>stems.onnx: Spleeter's separation network
(Deezer Research, MIT) as an ONNX graph without its weights. Each weight is
external data naming its bytes inside the official checkpoint's
model.data-00000-of-00001 (offset and length read from model.index), which
Montage unpacks from the official <n>stems.tar.gz on first use, so the
weights come only from their authors.

  python3 scripts/gen-spleeter-graph.py <unpacked n stems folder> <n> src/media/spleeter-<n>stems.onnx

Needs onnx and numpy only. The network is spleeter/model/functions/unet.py
(2.1-2.4, unchanged since 1.4), transcribed, with the ratio masks of
spleeter/model/__init__.py _build_masks before their extension to the full
spectrum:

  input  "magnitude" [N, 2, T, 1024]: |STFT| of the mix (channels, frames, bins), T a multiple of 64
  output "masks"     [N, S, 2, T, 1024]: one ratio mask per stem, summing to one
"""
import struct
import sys

import numpy as np
import onnx
from onnx import TensorProto, helper

STEMS = {
    2: (["vocals", "accompaniment"], "unet", "LeakyRelu", "Relu"),
    4: (["vocals", "drums", "bass", "other"], "unet", "Elu", "Elu"),
    5: (["vocals", "piano", "drums", "bass", "other"], "softmax_unet", "Elu", "Elu"),
}
FILTERS = [16, 32, 64, 128, 256, 512]
EPSILON = 1e-10
BN_EPSILON = 1e-3  # Keras' BatchNormalization default
DATA = "model.data-00000-of-00001"


# --- TensorFlow's tensor bundle index: a LevelDB table of BundleEntryProto ---

def varint(b, i):
    v, shift = 0, 0
    while True:
        c = b[i]
        i += 1
        v |= (c & 0x7F) << shift
        if c < 0x80:
            return v, i
        shift += 7


def block_entries(b):
    restarts = struct.unpack_from("<I", b, len(b) - 4)[0]
    end = len(b) - 4 - 4 * restarts
    i, key = 0, b""
    while i < end:
        shared, i = varint(b, i)
        unshared, i = varint(b, i)
        vlen, i = varint(b, i)
        key = key[:shared] + b[i:i + unshared]
        i += unshared
        yield key, b[i:i + vlen]
        i += vlen


def read_block(data, off, size):
    assert data[off + size] == 0, "compressed index block"
    return data[off:off + size]


def bundle_entry(v):
    """dtype, shape, shard, offset, size of a BundleEntryProto."""
    out = {"shape": [], "shard": 0, "offset": 0, "size": 0, "dtype": 0}
    i = 0
    while i < len(v):
        tag, i = varint(v, i)
        field, wire = tag >> 3, tag & 7
        if wire == 0:
            val, i = varint(v, i)
            out[{1: "dtype", 3: "shard", 4: "offset", 5: "size"}.get(field, "_")] = val
        elif wire == 2:
            n, i = varint(v, i)
            sub = v[i:i + n]
            i += n
            if field == 2:  # TensorShapeProto: repeated Dim dim = 2 { int64 size = 1 }
                j = 0
                while j < len(sub):
                    t, j = varint(sub, j)
                    m, j = varint(sub, j)
                    dim, k = sub[j:j + m], 0
                    j += m
                    while k < len(dim):
                        t2, k = varint(dim, k)
                        if t2 == 8:
                            size, k = varint(dim, k)
                            out["shape"].append(size)
                        else:
                            _, k = varint(dim, k)
        elif wire == 5:
            i += 4
        elif wire == 1:
            i += 8
    return out


def read_index(path):
    data = open(path, "rb").read()
    footer = data[-48:]
    assert struct.unpack_from("<Q", footer, 40)[0] == 0xDB4775248B80FB57, "not a TensorFlow index"
    j = 0
    _, j = varint(footer, j)
    _, j = varint(footer, j)  # metaindex handle
    ioff, j = varint(footer, j)
    isize, j = varint(footer, j)
    entries = {}
    for _, handle in block_entries(read_block(data, ioff, isize)):
        boff, k = varint(handle, 0)
        bsize, k = varint(handle, k)
        for key, value in block_entries(read_block(data, boff, bsize)):
            if key:
                entries[key.decode()] = bundle_entry(value)
    return entries


# --- the graph ---

class Graph:
    def __init__(self, index):
        self.index = index
        self.nodes, self.inits, self.n = [], [], 0

    def name(self, prefix):
        self.n += 1
        return f"{prefix}_{self.n}"

    def op(self, kind, inputs, **attrs):
        out = self.name(kind.lower())
        self.nodes.append(helper.make_node(kind, inputs, [out], **attrs))
        return out

    def const(self, values, dtype=np.int64):
        name = self.name("const")
        self.inits.append(onnx.numpy_helper.from_array(np.array(values, dtype=dtype), name))
        return name

    def weight(self, var):
        e = self.index[var]
        assert e["dtype"] == 1 and e["shard"] == 0, var  # DT_FLOAT in the one shard
        assert e["size"] == 4 * int(np.prod(e["shape"])), var
        init = TensorProto()
        init.name, init.data_type = var, TensorProto.FLOAT
        init.dims.extend(e["shape"])
        init.data_location = TensorProto.EXTERNAL
        for k, v in (("location", DATA), ("offset", str(e["offset"])), ("length", str(e["size"]))):
            x = init.external_data.add()
            x.key, x.value = k, v
        self.inits.append(init)
        return var


def layer(base, k):
    return base if k == 0 else f"{base}_{k}"


def unet(g, x, i, conv_act, deconv_act, logits):
    """One stem's U-Net (apply_unet); layers are numbered per stem in creation order."""
    convs, bns, ups = iter(range(7 * i, 7 * i + 7)), iter(range(12 * i, 12 * i + 12)), iter(range(6 * i, 6 * i + 6))

    def act(kind, t):
        return g.op("LeakyRelu", [t], alpha=0.2) if kind == "LeakyRelu" else g.op(kind, [t])

    def bn(t):
        n = layer("batch_normalization", next(bns))
        return g.op("BatchNormalization", [t, g.weight(n + "/gamma"), g.weight(n + "/beta"),
                                          g.weight(n + "/moving_mean"), g.weight(n + "/moving_variance")], epsilon=BN_EPSILON)

    def conv(t, k=5, stride=2, dilation=1):
        n = layer("conv2d", next(convs))
        w = g.op("Transpose", [g.weight(n + "/kernel")], perm=[3, 2, 0, 1])  # HWIO -> OIHW
        eff = (k - 1) * dilation + 1
        # TensorFlow's "same": on even sizes, stride 2 pads one before and two after; stride 1 pads equally.
        total = eff - stride
        pads = [total // 2, total // 2, total - total // 2, total - total // 2]
        return g.op("Conv", [t, w, g.weight(n + "/bias")], kernel_shape=[k, k], strides=[stride, stride],
                    dilations=[dilation, dilation], pads=pads)

    def deconv(t):
        n = layer("conv2d_transpose", next(ups))
        w = g.op("Transpose", [g.weight(n + "/kernel")], perm=[3, 2, 0, 1])  # H W out in -> in out H W
        # The gradient of a "same" stride-2 convolution: the full output cropped by one before and two after.
        return g.op("ConvTranspose", [t, w, g.weight(n + "/bias")], kernel_shape=[5, 5], strides=[2, 2], pads=[1, 1, 2, 2])

    encoded, t = [], x
    for _ in range(6):
        c = conv(t)
        encoded.append(c)
        t = act(conv_act, bn(c))
    t = encoded[5]  # the decoder starts from the sixth convolution before its normalisation, as Spleeter's does
    for level in range(6):
        t = bn(act(deconv_act, deconv(t)))
        if level < 5:
            t = g.op("Concat", [encoded[4 - level], t], axis=1)  # each convolution's output before normalisation
    out = conv(t, k=4, stride=1, dilation=2)
    return out if logits else g.op("Sigmoid", [out])


def main(folder, stems, out_path):
    names, kind, conv_act, deconv_act = STEMS[stems]
    index = read_index(folder + "/model.index")
    g = Graph(index)
    x = "magnitude"
    if kind == "unet":
        outs = [g.op("Mul", [unet(g, x, i, conv_act, deconv_act, False), x]) for i in range(stems)]
    else:  # softmax_unet: a softmax across the stems' logits, then each times the mix
        stacked = g.op("Concat", [g.op("Unsqueeze", [unet(g, x, i, conv_act, deconv_act, True), g.const([1])])
                                  for i in range(stems)], axis=1)
        soft = g.op("Softmax", [stacked], axis=1)
        outs = [g.op("Mul", [g.op("Squeeze", [g.op("Slice", [soft, g.const([i]), g.const([i + 1]), g.const([1])]),
                                              g.const([1])]), x]) for i in range(stems)]
    # Ratio masks (separation exponent 2): (out^2 + eps/S) / (sum out^2 + eps).
    squares = [g.op("Mul", [o, o]) for o in outs]
    total = squares[0]
    for s in squares[1:]:
        total = g.op("Add", [total, s])
    total = g.op("Add", [total, g.const(EPSILON, np.float32)])
    masks = [g.op("Div", [g.op("Add", [s, g.const(EPSILON / stems, np.float32)]), total]) for s in squares]
    stacked = g.op("Concat", [g.op("Unsqueeze", [m, g.const([1])]) for m in masks], axis=1)
    g.nodes.append(helper.make_node("Identity", [stacked], ["masks"]))
    graph = helper.make_graph(
        g.nodes, f"spleeter-{stems}stems",
        [helper.make_tensor_value_info("magnitude", TensorProto.FLOAT, ["n", 2, "frames", 1024])],
        [helper.make_tensor_value_info("masks", TensorProto.FLOAT, ["n", stems, 2, "frames", 1024])],
        g.inits)
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)], producer_name="montage gen-spleeter-graph.py")
    model.ir_version = 8
    model.doc_string = f"Spleeter {stems} stems ({', '.join(names)}; Deezer Research, MIT); weights: the official {stems}stems.tar.gz's {DATA}"
    onnx.shape_inference.infer_shapes(model, check_type=True, strict_mode=True)  # the weights are not here to check
    onnx.save(model, out_path)
    external = sum(1 for i in g.inits if i.data_location == TensorProto.EXTERNAL)
    print(f"{out_path}: {len(g.nodes)} nodes, {external} external weights, {len(model.SerializeToString())} bytes")


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]), sys.argv[3])
