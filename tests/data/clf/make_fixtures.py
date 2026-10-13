# Writes the CLF test fixtures: python3 make_fixtures.py tests/data/clf
import math, sys
out = sys.argv[1]
H = '<?xml version="1.0" encoding="UTF-8"?>\n'
def write(name, body, pname):
    open(f"{out}/{name}", "w").write(H + f'<ProcessList compCLFversion="3.0" id="{pname}" name="{pname}">\n    <Description>Montage test fixture: {pname}</Description>\n' + body + '</ProcessList>\n')
ops = '''    <Matrix inBitDepth="10i" outBitDepth="10i">
        <Array dim="3 4">
            0.9 0.08 0.02 10.23
            0.05 0.9 0.05 0
            0.01 0.1 0.89 -5.115
        </Array>
    </Matrix>
    <Range inBitDepth="10i" outBitDepth="32f">
        <minInValue>-51.15</minInValue>
        <maxInValue>1125.3</maxInValue>
        <minOutValue>0</minOutValue>
        <maxOutValue>1</maxOutValue>
    </Range>
    <Log inBitDepth="32f" outBitDepth="32f" style="cameraLinToLog">
        <LogParams base="2" logSideSlope="0.0570776255707763" logSideOffset="0.554794520547945" linSideSlope="1" linSideOffset="0" linSideBreak="0.0078125" />
    </Log>
    <ASC_CDL inBitDepth="32f" outBitDepth="32f" style="Fwd">
        <SOPNode>
            <Slope>1.1 0.95 1.02</Slope>
            <Offset>-0.02 0.01 0.03</Offset>
            <Power>0.9 1.05 1.1</Power>
        </SOPNode>
        <SatNode>
            <Saturation>1.2</Saturation>
        </SatNode>
    </ASC_CDL>
    <Exponent inBitDepth="32f" outBitDepth="32f" style="monCurveFwd">
        <ExponentParams exponent="2.4" offset="0.055" />
    </Exponent>
'''
n = 32
lut = []
for i in range(n):
    x = i / (n - 1)
    lut.append((round(4095 * x ** 0.8), round(4095 * (0.5 - 0.5 * math.cos(math.pi * x))), round(4095 * min(1, x * 1.1))))
ops += '    <LUT1D inBitDepth="32f" outBitDepth="12i">\n        <Array dim="%d 3">\n' % n + ''.join('            %d %d %d\n' % t for t in lut) + '        </Array>\n    </LUT1D>\n'
ops += '''    <Matrix inBitDepth="12i" outBitDepth="32f">
        <Array dim="3 3">
            0.000244200244 0 0
            0 0.000244200244 0
            0 0 0.000244200244
        </Array>
    </Matrix>
    <Log inBitDepth="32f" outBitDepth="32f" style="linToLog">
        <LogParams base="10" logSideSlope="0.25" logSideOffset="0.6" linSideSlope="1.5" linSideOffset="0.01" />
    </Log>
    <Exponent inBitDepth="32f" outBitDepth="32f" style="basicMirrorRev">
        <ExponentParams channel="R" exponent="1.2" />
        <ExponentParams channel="G" exponent="1.4" />
        <ExponentParams channel="B" exponent="1.6" />
    </Exponent>
'''
write("operators.clf", ops, "operators")

def lut3d(interp, depth, scale):
    n = 9
    s = f'    <LUT3D inBitDepth="32f" outBitDepth="{depth}" interpolation="{interp}">\n        <Array dim="{n} {n} {n} 3">\n'
    for r in range(n):
        for g in range(n):
            for b in range(n):
                R, G, B = r / (n - 1), g / (n - 1), b / (n - 1)
                y = 0.3 * R + 0.6 * G + 0.1 * B
                o = (min(1, R ** 1.3 * 0.9 + 0.1 * B), min(1, 0.5 * G + 0.5 * y), min(1, (B * 0.8 + 0.2 * R) ** 0.7))
                if scale == 1: s += '            %.6f %.6f %.6f\n' % o
                else: s += '            %d %d %d\n' % tuple(round(v * scale) for v in o)
    return s + '        </Array>\n    </LUT3D>\n'
write("lut3d_tetrahedral.clf", lut3d("tetrahedral", "10i", 1023), "tetrahedral")
write("lut3d_trilinear.clf", '''    <Range inBitDepth="32f" outBitDepth="32f" style="noClamp">
        <minInValue>0</minInValue>
        <maxInValue>2</maxInValue>
        <minOutValue>0</minOutValue>
        <maxOutValue>1</maxOutValue>
    </Range>
''' + lut3d("trilinear", "32f", 1) + '''    <ASC_CDL inBitDepth="32f" outBitDepth="32f" style="RevNoClamp">
        <SOPNode>
            <Slope>1.2 1 0.9</Slope>
            <Offset>0.01 0 -0.01</Offset>
            <Power>1.1 1 0.95</Power>
        </SOPNode>
        <SatNode>
            <Saturation>0.8</Saturation>
        </SatNode>
    </ASC_CDL>
''', "trilinear")

# The operators and styles the first file leaves out, with values kept outside 0..1 where the operators allow.
more = '''    <Matrix inBitDepth="32f" outBitDepth="32f">
        <Array dim="3 4">
            1.2 -0.1 0 -0.05
            0 1.1 0 0.02
            -0.1 0 1.3 0
        </Array>
    </Matrix>
    <Exponent inBitDepth="32f" outBitDepth="32f" style="basicPassThruFwd">
        <ExponentParams exponent="1.3" />
    </Exponent>
    <Exponent inBitDepth="32f" outBitDepth="32f" style="basicMirrorFwd">
        <ExponentParams exponent="0.9" />
    </Exponent>
    <ASC_CDL inBitDepth="32f" outBitDepth="32f" style="FwdNoClamp">
        <SOPNode>
            <Slope>1.05 0.97 1.1</Slope>
            <Offset>0.01 -0.02 0.0</Offset>
            <Power>1.1 0.95 1.0</Power>
        </SOPNode>
        <SatNode>
            <Saturation>1.1</Saturation>
        </SatNode>
    </ASC_CDL>
    <Range inBitDepth="32f" outBitDepth="32f" style="noClamp">
        <minInValue>-0.2</minInValue>
        <maxInValue>1.4</maxInValue>
        <minOutValue>0.05</minOutValue>
        <maxOutValue>0.9</maxOutValue>
    </Range>
    <Log inBitDepth="32f" outBitDepth="32f" style="cameraLogToLin">
        <LogParams base="2" logSideSlope="0.0570776255707763" logSideOffset="0.554794520547945" linSideSlope="1" linSideOffset="0" linSideBreak="0.0078125" />
    </Log>
    <Range inBitDepth="32f" outBitDepth="32f">
        <minInValue>0.001</minInValue>
        <minOutValue>0.001</minOutValue>
    </Range>
    <Log inBitDepth="32f" outBitDepth="32f" style="log10" />
    <Log inBitDepth="32f" outBitDepth="32f" style="antiLog10" />
    <Log inBitDepth="32f" outBitDepth="32f" style="log2" />
    <Log inBitDepth="32f" outBitDepth="32f" style="antiLog2" />
    <Log inBitDepth="32f" outBitDepth="32f" style="linToLog">
        <LogParams base="2" logSideSlope="0.08" logSideOffset="0.6" linSideSlope="1" linSideOffset="0.02" />
    </Log>
    <Log inBitDepth="32f" outBitDepth="32f" style="logToLin">
        <LogParams base="2" logSideSlope="0.09" logSideOffset="0.55" linSideSlope="1.2" linSideOffset="0.01" />
    </Log>
    <Exponent inBitDepth="32f" outBitDepth="32f" style="monCurveRev">
        <ExponentParams exponent="2.4" offset="0.055" />
    </Exponent>
    <Exponent inBitDepth="32f" outBitDepth="32f" style="basicFwd">
        <ExponentParams exponent="1.1" />
    </Exponent>
    <ASC_CDL inBitDepth="32f" outBitDepth="32f" style="Rev">
        <SOPNode>
            <Slope>1.1 1.0 0.95</Slope>
            <Offset>0.0 0.01 -0.01</Offset>
            <Power>1.0 1.05 0.9</Power>
        </SOPNode>
        <SatNode>
            <Saturation>0.9</Saturation>
        </SatNode>
    </ASC_CDL>
'''
more += '    <LUT1D inBitDepth="32f" outBitDepth="16i">\n        <Array dim="17 1">\n' + ''.join('            %d\n' % round(65535 * (i / 16) ** 1.4) for i in range(17)) + '        </Array>\n    </LUT1D>\n'
write("more_operators.clf", more, "more operators")
