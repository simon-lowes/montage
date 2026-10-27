#!/usr/bin/env python3
"""Writes the USD test scenes and their glTF twin, for the 3D model tests.

cube.usda and cube.gltf: the same 2 x 2 x 2 cube, each face's corners its own vertices with uvs over the whole face,
in a material whose base colour is checker.png (an 8 x 8 sRGB checker of red and blue), roughness 0.4, not metallic.
anim.usda: a prim whose translation runs through (0, 0, 0), (1, 0, 0) and (1, 2, 0) at time codes 0, 12 and 24 (24
a second) while it turns 0 to 90 degrees about y. camera.usda: the cube (referenced from cube.usda) seen from 8 units
down +z by a 50 mm lens on a 36 x 20.25 mm film back, lit by a distant light straight down -z of illuminance pi.
"""
import base64
import json
import struct
import zlib
from pathlib import Path

here = Path(__file__).parent

# The checker: 2 x 2 pixel squares, red where (x // 2 + y // 2) is even.
rows = b""
for y in range(8):
    rows += b"\0" + b"".join(bytes((220, 40, 30) if (x // 2 + y // 2) % 2 == 0 else (30, 60, 200)) for x in range(8))


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 8, 8, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
(here / "checker.png").write_bytes(png)

# The cube's faces, corners anticlockwise from outside: (normal axis, sign) -> corners.
corners = {(0, 1): [(1, -1, 1), (1, -1, -1), (1, 1, -1), (1, 1, 1)], (0, -1): [(-1, -1, -1), (-1, -1, 1), (-1, 1, 1), (-1, 1, -1)],
           (1, 1): [(-1, 1, 1), (1, 1, 1), (1, 1, -1), (-1, 1, -1)], (1, -1): [(-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1)],
           (2, 1): [(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)], (2, -1): [(1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1)]}
st = [(0, 0), (1, 0), (1, 1), (0, 1)]  # USD's (t up the picture)
points = [p for face in corners.values() for p in face]


def tup(v):
    return "(" + ", ".join(repr(float(x)) for x in v) + ")"


usda = f'''#usda 1.0
(
    defaultPrim = "Cube"
    upAxis = "Y"
    metersPerUnit = 1
)

def Mesh "Cube" (
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{{
    int[] faceVertexCounts = [{", ".join("4" for _ in corners)}]
    int[] faceVertexIndices = [{", ".join(str(i) for i in range(len(points)))}]
    point3f[] points = [{", ".join(tup(p) for p in points)}]
    texCoord2f[] primvars:st = [{", ".join(tup(t) for _ in corners for t in st)}] (
        interpolation = "faceVarying"
    )
    uniform token subdivisionScheme = "none"
    rel material:binding = </Cube/Looks/Checker>

    def Scope "Looks"
    {{
        def Material "Checker"
        {{
            token outputs:surface.connect = </Cube/Looks/Checker/Surface.outputs:surface>

            def Shader "Surface"
            {{
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor.connect = </Cube/Looks/Checker/Texture.outputs:rgb>
                float inputs:roughness = 0.4
                float inputs:metallic = 0
                token outputs:surface
            }}

            def Shader "Texture"
            {{
                uniform token info:id = "UsdUVTexture"
                asset inputs:file = @checker.png@
                token inputs:sourceColorSpace = "sRGB"
                float2 inputs:st.connect = </Cube/Looks/Checker/Reader.outputs:result>
                float3 outputs:rgb
            }}

            def Shader "Reader"
            {{
                uniform token info:id = "UsdPrimvarReader_float2"
                string inputs:varname = "st"
                float2 outputs:result
            }}
        }}
    }}
}}
'''
(here / "cube.usda").write_text(usda)

# The glTF twin: the same corners, uvs with v down the picture.
pos = b"".join(struct.pack("<3f", *p) for p in points)
uv = b"".join(struct.pack("<2f", s, 1 - t) for _ in corners for s, t in st)
idx = b"".join(struct.pack("<3H", 4 * f, 4 * f + a, 4 * f + a + 1) for f in range(6) for a in (1, 2))
buf = pos + uv + idx
gltf = {
    "asset": {"version": "2.0"},
    "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": "Cube"}],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1}, "indices": 2, "material": 0}]}],
    "materials": [{"name": "Checker", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0, "roughnessFactor": 0.4}}],
    "textures": [{"source": 0, "sampler": 0}], "samplers": [{}], "images": [{"uri": "checker.png"}],
    "buffers": [{"byteLength": len(buf), "uri": "data:application/octet-stream;base64," + base64.b64encode(buf).decode()}],
    "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": len(pos)}, {"buffer": 0, "byteOffset": len(pos), "byteLength": len(uv)},
                    {"buffer": 0, "byteOffset": len(pos) + len(uv), "byteLength": len(idx)}],
    "accessors": [{"bufferView": 0, "componentType": 5126, "count": len(points), "type": "VEC3", "min": [-1, -1, -1], "max": [1, 1, 1]},
                  {"bufferView": 1, "componentType": 5126, "count": len(points), "type": "VEC2"},
                  {"bufferView": 2, "componentType": 5123, "count": 36, "type": "SCALAR"}],
}
(here / "cube.gltf").write_text(json.dumps(gltf, indent=1) + "\n")

(here / "anim.usda").write_text('''#usda 1.0
(
    defaultPrim = "Mover"
    startTimeCode = 0
    endTimeCode = 24
    timeCodesPerSecond = 24
    framesPerSecond = 24
)

def Xform "Mover"
{
    double3 xformOp:translate.timeSamples = {
        0: (0, 0, 0),
        12: (1, 0, 0),
        24: (1, 2, 0),
    }
    float xformOp:rotateY.timeSamples = {
        0: 0,
        24: 90,
    }
    uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateY"]

    def Mesh "Triangle"
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        uniform token subdivisionScheme = "none"
    }
}
''')

(here / "camera.usda").write_text('''#usda 1.0
(
    upAxis = "Y"
)

def "Box" (
    references = @cube.usda@
)
{
}

def Camera "Cam"
{
    float focalLength = 50
    float horizontalAperture = 36
    float verticalAperture = 20.25
    float2 clippingRange = (0.1, 1000)
    double3 xformOp:translate = (0, 0, 8)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}

def DistantLight "Sun"
{
    float inputs:intensity = 3.14159265
    float inputs:angle = 0
    bool inputs:shadow:enable = 0
}
''')
