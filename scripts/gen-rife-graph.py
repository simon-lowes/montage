#!/usr/bin/env python3
"""Generates src/media/rife-4.26.onnx: RIFE 4.26's network (Practical-RIFE,
Zhewei Huang et al., MIT) as an ONNX graph without its weights. Each weight
is external data naming the raw tensor file inside the official
flownet.pkl ("data/<key>"), which Montage unpacks from the official
RIFEv4.26_0921.zip on first use, so the weights come only from their
authors.

  python3 scripts/gen-rife-graph.py RIFEv4.26_0921.zip src/media/rife-4.26.onnx

Needs torch and onnx. The network below is IFNet_HDv3.py from the release
(4.26, fast mode, no ensemble) with warplayer.py's warp, transcribed.
"""
import io
import sys
import zipfile

import onnx
import torch
import torch.nn as nn
import torch.nn.functional as F
from onnx import numpy_helper


def warp(ten_input, ten_flow):
    n, _, h, w = ten_flow.shape
    horizontal = torch.linspace(-1.0, 1.0, w).view(1, 1, 1, w).expand(n, -1, h, -1)
    vertical = torch.linspace(-1.0, 1.0, h).view(1, 1, h, 1).expand(n, -1, -1, w)
    grid = torch.cat([horizontal, vertical], 1)
    ten_flow = torch.cat([ten_flow[:, 0:1] / ((ten_input.shape[3] - 1.0) / 2.0),
                          ten_flow[:, 1:2] / ((ten_input.shape[2] - 1.0) / 2.0)], 1)
    g = (grid + ten_flow).permute(0, 2, 3, 1)
    return F.grid_sample(ten_input, g, mode="bilinear", padding_mode="border", align_corners=True)


def conv(cin, cout, k=3, s=1, p=1, d=1):
    return nn.Sequential(nn.Conv2d(cin, cout, k, s, p, dilation=d, bias=True), nn.LeakyReLU(0.2, True))


class Head(nn.Module):
    def __init__(self):
        super().__init__()
        self.cnn0 = nn.Conv2d(3, 16, 3, 2, 1)
        self.cnn1 = nn.Conv2d(16, 16, 3, 1, 1)
        self.cnn2 = nn.Conv2d(16, 16, 3, 1, 1)
        self.cnn3 = nn.ConvTranspose2d(16, 4, 4, 2, 1)
        self.relu = nn.LeakyReLU(0.2, True)

    def forward(self, x):
        x = self.relu(self.cnn0(x))
        x = self.relu(self.cnn1(x))
        x = self.relu(self.cnn2(x))
        return self.cnn3(x)


class ResConv(nn.Module):
    def __init__(self, c, dilation=1):
        super().__init__()
        self.conv = nn.Conv2d(c, c, 3, 1, dilation, dilation=dilation, groups=1)
        self.beta = nn.Parameter(torch.ones((1, c, 1, 1)))
        self.relu = nn.LeakyReLU(0.2, True)

    def forward(self, x):
        return self.relu(self.conv(x) * self.beta + x)


class IFBlock(nn.Module):
    def __init__(self, cin, c=64):
        super().__init__()
        self.conv0 = nn.Sequential(conv(cin, c // 2, 3, 2, 1), conv(c // 2, c, 3, 2, 1))
        self.convblock = nn.Sequential(*[ResConv(c) for _ in range(8)])
        self.lastconv = nn.Sequential(nn.ConvTranspose2d(c, 4 * 13, 4, 2, 1), nn.PixelShuffle(2))

    def forward(self, x, flow, scale):
        x = F.interpolate(x, scale_factor=1.0 / scale, mode="bilinear", align_corners=False)
        if flow is not None:
            flow = F.interpolate(flow, scale_factor=1.0 / scale, mode="bilinear", align_corners=False) * 1.0 / scale
            x = torch.cat((x, flow), 1)
        feat = self.convblock(self.conv0(x))
        tmp = F.interpolate(self.lastconv(feat), scale_factor=scale, mode="bilinear", align_corners=False)
        return tmp[:, :4] * scale, tmp[:, 4:5], tmp[:, 5:]


class IFNet(nn.Module):
    def __init__(self, scale=1.0):
        super().__init__()
        self.block0 = IFBlock(7 + 8, c=192)
        self.block1 = IFBlock(8 + 4 + 8 + 8, c=128)
        self.block2 = IFBlock(8 + 4 + 8 + 8, c=96)
        self.block3 = IFBlock(8 + 4 + 8 + 8, c=64)
        self.block4 = IFBlock(8 + 4 + 8 + 8, c=32)
        self.encode = Head()
        self.scales = [16 / scale, 8 / scale, 4 / scale, 2 / scale, 1 / scale]

    def forward(self, img0, img1, timestep):
        timestep = timestep.repeat(1, 1, img0.shape[2], img0.shape[3])
        f0, f1 = self.encode(img0), self.encode(img1)
        blocks = [self.block0, self.block1, self.block2, self.block3, self.block4]
        flow = mask = feat = None
        warped0, warped1 = img0, img1
        for i in range(5):
            if flow is None:
                flow, mask, feat = blocks[i](torch.cat((img0, img1, f0, f1, timestep), 1), None, self.scales[i])
            else:
                wf0, wf1 = warp(f0, flow[:, :2]), warp(f1, flow[:, 2:4])
                fd, mask, feat = blocks[i](torch.cat((warped0, warped1, wf0, wf1, timestep, mask, feat), 1), flow, self.scales[i])
                flow = flow + fd
            warped0, warped1 = warp(img0, flow[:, :2]), warp(img1, flow[:, 2:4])
        mask = torch.sigmoid(mask)
        return warped0 * mask + warped1 * (1 - mask)


def main(zip_path, out_path, scale=1.0):
    outer = zipfile.ZipFile(zip_path)
    pkl = outer.read("RIFEv4.26_0921/flownet.pkl")
    inner = zipfile.ZipFile(io.BytesIO(pkl))
    blobs = {n.split("/data/")[1]: inner.read(n) for n in inner.namelist() if "/data/" in n}
    state = torch.load(io.BytesIO(pkl), map_location="cpu", weights_only=True)
    state = {k.replace("module.", ""): v for k, v in state.items()}
    net = IFNet(scale)
    missing, unexpected = net.load_state_dict(state, strict=False)
    assert not missing, missing
    net.eval()
    x0, x1, t = torch.rand(1, 3, 128, 192), torch.rand(1, 3, 128, 192), torch.full((1, 1, 1, 1), 0.5)
    buf = io.BytesIO()
    with torch.no_grad():
        torch.onnx.export(net, (x0, x1, t), buf, opset_version=17, do_constant_folding=False, dynamo=False,
                          input_names=["img0", "img1", "timestep"], output_names=["frame"],
                          dynamic_axes={"img0": {2: "h", 3: "w"}, "img1": {2: "h", 3: "w"}, "frame": {2: "h", 3: "w"}})
    model = onnx.load_from_string(buf.getvalue())
    # Each weight points at the official tensor file holding exactly its bytes.
    by_bytes = {}
    for key, data in blobs.items():
        by_bytes.setdefault(data, key)
    external = 0
    for init in model.graph.initializer:
        raw = numpy_helper.to_array(init).astype("<f4").tobytes() if init.data_type == onnx.TensorProto.FLOAT else None
        if raw is None or len(raw) < 64:
            continue
        key = by_bytes.get(raw)
        assert key is not None, "no official tensor for " + init.name
        init.ClearField("raw_data")
        init.ClearField("float_data")
        init.data_location = onnx.TensorProto.EXTERNAL
        for k, v in (("location", "data/" + key), ("offset", "0"), ("length", str(len(raw)))):
            e = init.external_data.add()
            e.key, e.value = k, v
        external += 1
    model.producer_name = "montage gen-rife-graph.py"
    model.doc_string = "RIFE 4.26 (Practical-RIFE, MIT); weights: the official flownet.pkl's data/ files"
    onnx.save(model, out_path)
    print(f"{out_path}: {len(model.graph.node)} nodes, {external} external weights, {len(model.SerializeToString())} bytes")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 1.0)
