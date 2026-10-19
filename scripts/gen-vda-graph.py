#!/usr/bin/env python3
"""Generates src/media/vda-small-{encoder,temporal,decoder}.onnx: Video Depth
Anything Small (Sili Chen et al., ByteDance, Apache-2.0) as three ONNX graphs
without their weights. Each weight is external data naming the raw tensor
file inside the official video_depth_anything_vits.pth ("data/<key>"), which
Montage unpacks on first use, so the weights come only from their authors.

  python3 scripts/gen-vda-graph.py video_depth_anything_vits.pth src/media

Needs torch and onnx. The network is the release's dinov2.py, dpt.py,
dpt_temporal.py, util/blocks.py and motion_module/, transcribed (ViT-S/14
encoder, DPT head, four temporal modules). It is split where its work
changes from per frame to per window and back, so no step holds the whole
window at full size:

  encoder   frames [N,3,H,W] (H, W multiples of 14, ImageNet-normalised)
            -> layer1 [N,48,h,w], layer2 [N,96,h,w], layer3 [N,192,h,w], layer4 [N,384,h,w] (the four
               feature maps, projected)
  temporal  layer3, layer4 for the 32 frames of a window -> path3 [32,64,2h,2w]
  decoder   path3, layer2, layer1 [N,...] -> depth [N,1,H,W] (relative disparity, nearer is larger)

where h, w = H/14, W/14. The position embedding is resized in the graph as
the original resizes it (bicubic, scale (h+0.1)/37).

Video Depth Anything: Copyright (2025) Bytedance Ltd. and/or its affiliates,
under the Apache License 2.0 (LICENSES/Apache-2.0.txt), as are DINOv2 (Meta
Platforms) and the motion module from AnimateDiff it builds on; modified:
rewritten as ONNX graph construction for Montage. See THIRD_PARTY_NOTICES.md.
"""
import io
import math
import os
import sys
import zipfile

import onnx
import torch
import torch.nn as nn
import torch.nn.functional as F
from onnx import numpy_helper

FRAMES = 32  # a window, as the model was trained


# ---- DINOv2 ViT-S/14 -------------------------------------------------------

class Attention(nn.Module):
    def __init__(self, dim, heads):
        super().__init__()
        self.heads = heads
        self.qkv = nn.Linear(dim, dim * 3)
        self.proj = nn.Linear(dim, dim)

    def forward(self, x):
        b, n, c = x.shape
        qkv = self.qkv(x).reshape(b, n, 3, self.heads, c // self.heads).permute(2, 0, 3, 1, 4)
        q, k, v = qkv[0] * (c // self.heads) ** -0.5, qkv[1], qkv[2]
        a = (q @ k.transpose(-2, -1)).softmax(dim=-1)
        return self.proj((a @ v).transpose(1, 2).reshape(b, n, c))


class LayerScale(nn.Module):
    def __init__(self, dim):
        super().__init__()
        self.gamma = nn.Parameter(torch.ones(dim))

    def forward(self, x):
        return x * self.gamma


class Mlp(nn.Module):
    def __init__(self, dim, hidden):
        super().__init__()
        self.fc1 = nn.Linear(dim, hidden)
        self.fc2 = nn.Linear(hidden, dim)

    def forward(self, x):
        return self.fc2(F.gelu(self.fc1(x)))


class Block(nn.Module):
    def __init__(self, dim, heads):
        super().__init__()
        self.norm1 = nn.LayerNorm(dim, eps=1e-6)
        self.attn = Attention(dim, heads)
        self.ls1 = LayerScale(dim)
        self.norm2 = nn.LayerNorm(dim, eps=1e-6)
        self.mlp = Mlp(dim, dim * 4)
        self.ls2 = LayerScale(dim)

    def forward(self, x):
        x = x + self.ls1(self.attn(self.norm1(x)))
        return x + self.ls2(self.mlp(self.norm2(x)))


class PatchEmbed(nn.Module):
    def __init__(self, dim):
        super().__init__()
        self.proj = nn.Conv2d(3, dim, 14, 14)

    def forward(self, x):
        return self.proj(x).flatten(2).transpose(1, 2)


def cubic_weights(n_out, n_in):
    """[n_out, n_in]: PyTorch's bicubic (A = -0.75, edges clamped) from n_in
    to n_out samples with scale factor (n_out + 0.1) / n_in, as a matrix, and
    the identity of that shape. n_out is a traced size, so the graph works at
    any picture size."""
    a = -0.75
    i = torch.arange(n_out).to(torch.float32)
    scale = (i[-1:] + 1.1) / n_in
    src = (i + 0.5) / scale - 0.5
    x0 = torch.floor(src)
    t = src - x0
    cols = torch.arange(n_in).to(torch.float32)
    m = torch.zeros_like(src).unsqueeze(1) * cols
    for k in range(4):
        d = torch.abs(t - (k - 1))
        w = torch.where(d <= 1, ((a + 2) * d - (a + 3)) * d * d + 1, ((a * d - 5 * a) * d + 8 * a) * d - 4 * a)
        idx = torch.clamp(x0 + (k - 1), 0, n_in - 1)
        m = m + w.unsqueeze(1) * (idx.unsqueeze(1) == cols).to(torch.float32)
    return m, (i.unsqueeze(1) == cols).to(torch.float32)


class DinoV2(nn.Module):
    def __init__(self, dim=384, depth=12, heads=6):
        super().__init__()
        self.cls_token = nn.Parameter(torch.zeros(1, 1, dim))
        self.pos_embed = nn.Parameter(torch.zeros(1, 1370, dim))
        self.mask_token = nn.Parameter(torch.zeros(1, dim))
        self.patch_embed = PatchEmbed(dim)
        self.blocks = nn.ModuleList([Block(dim, heads) for _ in range(depth)])
        self.norm = nn.LayerNorm(dim, eps=1e-6)

    def forward(self, x, layers=(2, 5, 8, 11)):
        tokens = self.patch_embed(x)
        n, c = 37, self.pos_embed.shape[-1]
        grid = self.pos_embed[:, 1:].reshape(n, n, c)
        # The position grid resized to the picture's patches (rows, then columns).
        rows, rows_same = cubic_weights(x.shape[2] // 14, n)
        cols, cols_same = cubic_weights(x.shape[3] // 14, n)
        # (A picture of the grid's own size takes it as it is.)
        same = (torch._shape_as_tensor(rows)[0] == n) & (torch._shape_as_tensor(cols)[0] == n)
        rows, cols = torch.where(same, rows_same, rows), torch.where(same, cols_same, cols)
        grid = torch.einsum("ri,ijc->rjc", rows, grid)
        grid = torch.einsum("sj,rjc->rsc", cols, grid).reshape(1, -1, c)
        pos = torch.cat((self.pos_embed[:, :1], grid), dim=1)
        x = torch.cat((self.cls_token.expand(tokens.shape[0], -1, -1), tokens), dim=1) + pos
        out = []
        for i, blk in enumerate(self.blocks):
            x = blk(x)
            if i in layers:
                out.append(self.norm(x)[:, 1:])
        return out


# ---- DPT head ---------------------------------------------------------------

class ResidualConvUnit(nn.Module):
    def __init__(self, f):
        super().__init__()
        self.conv1 = nn.Conv2d(f, f, 3, 1, 1)
        self.conv2 = nn.Conv2d(f, f, 3, 1, 1)

    def forward(self, x):
        return self.conv2(F.relu(self.conv1(F.relu(x)))) + x


class FeatureFusionBlock(nn.Module):
    def __init__(self, f):
        super().__init__()
        self.out_conv = nn.Conv2d(f, f, 1)
        self.resConfUnit1 = ResidualConvUnit(f)
        self.resConfUnit2 = ResidualConvUnit(f)

    def forward(self, x, skip=None, size=None):
        if skip is not None:
            x = x + self.resConfUnit1(skip)
        x = self.resConfUnit2(x)
        if size is None:
            x = F.interpolate(x, scale_factor=2.0, mode="bilinear", align_corners=True)
        else:
            x = F.interpolate(x, size=size, mode="bilinear", align_corners=True)
        return self.out_conv(x)


class Scratch(nn.Module):
    def __init__(self, chans, f):
        super().__init__()
        self.layer1_rn = nn.Conv2d(chans[0], f, 3, 1, 1, bias=False)
        self.layer2_rn = nn.Conv2d(chans[1], f, 3, 1, 1, bias=False)
        self.layer3_rn = nn.Conv2d(chans[2], f, 3, 1, 1, bias=False)
        self.layer4_rn = nn.Conv2d(chans[3], f, 3, 1, 1, bias=False)
        self.refinenet1 = FeatureFusionBlock(f)
        self.refinenet2 = FeatureFusionBlock(f)
        self.refinenet3 = FeatureFusionBlock(f)
        self.refinenet4 = FeatureFusionBlock(f)
        self.output_conv1 = nn.Conv2d(f, f // 2, 3, 1, 1)
        self.output_conv2 = nn.Sequential(nn.Conv2d(f // 2, 32, 3, 1, 1), nn.ReLU(), nn.Conv2d(32, 1, 1), nn.ReLU())


# ---- Temporal modules (AnimateDiff's motion module, "ape" positions) --------

class TemporalAttention(nn.Module):
    def __init__(self, dim, heads=8):
        super().__init__()
        self.heads = heads
        self.to_q = nn.Linear(dim, dim, bias=False)
        self.to_k = nn.Linear(dim, dim, bias=False)
        self.to_v = nn.Linear(dim, dim, bias=False)
        self.to_out = nn.ModuleList([nn.Linear(dim, dim)])
        self.pos_encoder = nn.Module()
        self.pos_encoder.register_buffer("pe", torch.zeros(1, FRAMES, dim))

    def forward(self, x):  # [T, P, C] -> attention across T for each of the P positions
        t, p, c = x.shape
        h = self.heads
        x = x.permute(1, 0, 2) + self.pos_encoder.pe
        q = self.to_q(x).reshape(p, t, h, c // h).permute(0, 2, 1, 3)
        k = self.to_k(x).reshape(p, t, h, c // h).permute(0, 2, 1, 3)
        v = self.to_v(x).reshape(p, t, h, c // h).permute(0, 2, 1, 3)
        a = ((q * (c // h) ** -0.5) @ k.transpose(-2, -1)).softmax(dim=-1)
        y = (a @ v).permute(0, 2, 1, 3).reshape(p, t, c)
        return self.to_out[0](y).permute(1, 0, 2)


class GEGLU(nn.Module):
    def __init__(self, dim, inner):
        super().__init__()
        self.proj = nn.Linear(dim, inner * 2)

    def forward(self, x):
        x, gate = self.proj(x).chunk(2, dim=-1)
        return x * F.gelu(gate)


class FeedForward(nn.Module):
    def __init__(self, dim):
        super().__init__()
        self.net = nn.ModuleList([GEGLU(dim, dim * 4), nn.Identity(), nn.Linear(dim * 4, dim)])

    def forward(self, x):
        return self.net[2](self.net[0](x))


class TransformerBlock(nn.Module):
    def __init__(self, dim):
        super().__init__()
        self.attention_blocks = nn.ModuleList([TemporalAttention(dim) for _ in range(2)])
        self.norms = nn.ModuleList([nn.LayerNorm(dim) for _ in range(2)])
        self.ff = FeedForward(dim)
        self.ff_norm = nn.LayerNorm(dim)

    def forward(self, x):
        for attn, norm in zip(self.attention_blocks, self.norms):
            x = attn(norm(x)) + x
        return self.ff(self.ff_norm(x)) + x


class TemporalTransformer(nn.Module):
    def __init__(self, c):
        super().__init__()
        self.norm = nn.GroupNorm(32, c, eps=1e-6)
        self.proj_in = nn.Linear(c, c)
        self.transformer_blocks = nn.ModuleList([TransformerBlock(c)])
        self.proj_out = nn.Linear(c, c)

    def forward(self, x):  # [T, C, H, W]
        t, c, h, w = x.shape
        y = self.norm(x).permute(0, 2, 3, 1).reshape(t, h * w, c)
        y = self.transformer_blocks[0](self.proj_in(y))
        return self.proj_out(y).reshape(t, h, w, c).permute(0, 3, 1, 2) + x


class TemporalModule(nn.Module):
    def __init__(self, c):
        super().__init__()
        self.temporal_transformer = TemporalTransformer(c)

    def forward(self, x):
        return self.temporal_transformer(x)


class Head(nn.Module):
    def __init__(self, dim=384, f=64, chans=(48, 96, 192, 384)):
        super().__init__()
        self.projects = nn.ModuleList([nn.Conv2d(dim, c, 1) for c in chans])
        self.resize_layers = nn.ModuleList([nn.ConvTranspose2d(chans[0], chans[0], 4, 4), nn.ConvTranspose2d(chans[1], chans[1], 2, 2),
                                            nn.Identity(), nn.Conv2d(chans[3], chans[3], 3, 2, 1)])
        self.scratch = Scratch(chans, f)
        self.motion_modules = nn.ModuleList([TemporalModule(chans[2]), TemporalModule(chans[3]), TemporalModule(f), TemporalModule(f)])


class VideoDepthAnything(nn.Module):
    def __init__(self):
        super().__init__()
        self.pretrained = DinoV2()
        self.head = Head()


# ---- The three graphs --------------------------------------------------------

class Encoder(nn.Module):
    def __init__(self, net):
        super().__init__()
        self.net = net

    def forward(self, frames):
        head = self.net.head
        feats = self.net.pretrained(frames)
        n, c = frames.shape[0], feats[0].shape[-1]
        out = []
        for i, x in enumerate(feats):
            x = x.permute(0, 2, 1).reshape(n, c, frames.shape[2] // 14, frames.shape[3] // 14)
            out.append(head.projects[i](x))
        return tuple(out)


class Temporal(nn.Module):
    def __init__(self, net):
        super().__init__()
        self.net = net

    def forward(self, layer3, layer4):
        head = self.net.head
        s = head.scratch
        l3 = s.layer3_rn(head.motion_modules[0](layer3))
        l4 = s.layer4_rn(head.motion_modules[1](head.resize_layers[3](layer4)))
        path4 = head.motion_modules[2](s.refinenet4(l4, size=l3.shape[2:]))
        size2 = (l3.shape[2] * 2, l3.shape[3] * 2)
        return head.motion_modules[3](s.refinenet3(path4, l3, size=size2))


class Decoder(nn.Module):
    def __init__(self, net):
        super().__init__()
        self.net = net

    def forward(self, path3, layer2, layer1):
        head = self.net.head
        s = head.scratch
        l1 = s.layer1_rn(head.resize_layers[0](layer1))
        l2 = s.layer2_rn(head.resize_layers[1](layer2))
        path2 = s.refinenet2(path3, l2, size=l1.shape[2:])
        path1 = s.refinenet1(path2, l1)
        out = s.output_conv1(path1)
        out = F.interpolate(out, size=(layer1.shape[2] * 14, layer1.shape[3] * 14), mode="bilinear", align_corners=True)
        return s.output_conv2(out)


def export(module, args, names, outputs, axes, by_bytes, out_path, doc):
    buf = io.BytesIO()
    with torch.no_grad():
        torch.onnx.export(module, args, buf, opset_version=17, do_constant_folding=False, dynamo=False,
                          input_names=names, output_names=outputs, dynamic_axes=axes)
    model = onnx.load_from_string(buf.getvalue())
    # Each weight points at the official tensor file holding exactly its bytes.
    external = 0
    for init in model.graph.initializer:
        raw = numpy_helper.to_array(init).astype("<f4").tobytes() if init.data_type == onnx.TensorProto.FLOAT else None
        if raw is None or len(raw) < 64:
            continue
        key = by_bytes.get(raw)
        if key is None:
            assert len(raw) <= 4096, "no official tensor for " + init.name
            continue
        init.ClearField("raw_data")
        init.ClearField("float_data")
        init.data_location = onnx.TensorProto.EXTERNAL
        for k, v in (("location", "data/" + key), ("offset", "0"), ("length", str(len(raw)))):
            e = init.external_data.add()
            e.key, e.value = k, v
        external += 1
    model.producer_name = "montage gen-vda-graph.py"
    model.doc_string = "Video Depth Anything Small (ByteDance, Apache-2.0), " + doc + "; weights: the official video_depth_anything_vits.pth's data/ files"
    onnx.save(model, out_path)
    print(f"{out_path}: {len(model.graph.node)} nodes, {external} external weights, {len(model.SerializeToString())} bytes")


def load(pth_path):
    z = zipfile.ZipFile(pth_path)
    blobs = {n.split("/data/")[1]: z.read(n) for n in z.namelist() if "/data/" in n}
    state = torch.load(pth_path, map_location="cpu", weights_only=True)
    net = VideoDepthAnything()
    net.load_state_dict(state, strict=True)
    net.eval()
    by_bytes = {}
    for key, data in blobs.items():
        by_bytes.setdefault(data, key)
    return net, by_bytes


def main(pth_path, out_dir):
    net, by_bytes = load(pth_path)
    h, w = 8, 12
    frames = torch.rand(2, 3, h * 14, w * 14)
    l1, l2, l3, l4 = Encoder(net)(frames)
    export(Encoder(net), (frames,), ["frames"], ["layer1", "layer2", "layer3", "layer4"],
           {"frames": {0: "n", 2: "h", 3: "w"}, **{f"layer{i}": {0: "n", 2: "ph", 3: "pw"} for i in range(1, 5)}},
           by_bytes, os.path.join(out_dir, "vda-small-encoder.onnx"), "encoder")
    l3w, l4w = l3[:1].expand(FRAMES, -1, -1, -1).contiguous(), l4[:1].expand(FRAMES, -1, -1, -1).contiguous()
    export(Temporal(net), (l3w, l4w), ["layer3", "layer4"], ["path3"],
           {"layer3": {2: "ph", 3: "pw"}, "layer4": {2: "ph", 3: "pw"}, "path3": {2: "h2", 3: "w2"}},
           by_bytes, os.path.join(out_dir, "vda-small-temporal.onnx"), "temporal modules")
    p3 = Temporal(net)(l3w, l4w)[:2]
    export(Decoder(net), (p3, l2, l1), ["path3", "layer2", "layer1"], ["depth"],
           {"path3": {0: "n", 2: "h2", 3: "w2"}, "layer2": {0: "n", 2: "ph", 3: "pw"}, "layer1": {0: "n", 2: "ph", 3: "pw"},
            "depth": {0: "n", 2: "h", 3: "w"}},
           by_bytes, os.path.join(out_dir, "vda-small-decoder.onnx"), "decoder")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
