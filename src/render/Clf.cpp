#include "Clf.h"

#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <map>
#include <mutex>
#include <sstream>

namespace montage {

// Every operator works on values normalised as the process list runs them: integer bit depths to 0..1, float ones as
// they are; an operator's own numbers are brought to that scale when it is read.
struct ClfTransform::Op {
    virtual ~Op() = default;
    virtual void apply(float v[3]) const = 0;
};

namespace {

double depthScale(const QString& depth) {
    if (depth == "8i") return 255;
    if (depth == "10i") return 1023;
    if (depth == "12i") return 4095;
    if (depth == "16i") return 65535;
    return 1;  // 16f, 32f
}

bool knownDepth(const QString& d) { return d == "8i" || d == "10i" || d == "12i" || d == "16i" || d == "16f" || d == "32f"; }

QDomElement child(const QDomElement& e, const char* name) {
    for (QDomElement c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
        if (c.localName() == name || c.tagName() == name) return c;
    return {};
}

bool numbers(const QString& text, std::vector<double>& out) {
    out.clear();
    std::istringstream is(text.toStdString());
    std::string tok;
    while (is >> tok) {
        char* end = nullptr;
        const double v = std::strtod(tok.c_str(), &end);
        if (!end || *end || !std::isfinite(v)) return false;
        out.push_back(v);
    }
    return true;
}

bool numberOf(const QDomElement& e, double& out) {
    std::vector<double> v;
    if (e.isNull() || !numbers(e.text(), v) || v.size() != 1) return false;
    out = v[0];
    return true;
}

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

struct MatrixOp : ClfTransform::Op {
    double m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};  // 3 x 4, offsets last
    void apply(float v[3]) const override {
        const double r = v[0], g = v[1], b = v[2];
        for (int i = 0; i < 3; ++i) v[i] = float(m[i * 4] * r + m[i * 4 + 1] * g + m[i * 4 + 2] * b + m[i * 4 + 3]);
    }
};

struct RangeOp : ClfTransform::Op {
    bool hasMin = false, hasMax = false, clamp = true;
    double scale = 1, offset = 0, minOut = 0, maxOut = 1;
    void apply(float v[3]) const override {
        for (int i = 0; i < 3; ++i) {
            double o = v[i] * scale + offset;
            if (clamp && hasMin) o = std::max(o, minOut);
            if (clamp && hasMax) o = std::min(o, maxOut);
            v[i] = float(o);
        }
    }
};

struct Lut1DOp : ClfTransform::Op {
    int n = 0, channels = 1;
    std::vector<float> table;  // n x channels, normalised
    void apply(float v[3]) const override {
        for (int i = 0; i < 3; ++i) {
            const float x = clamp01(v[i]) * float(n - 1);
            const int k = std::min(n - 2, int(x));
            const float f = x - float(k);
            const int c = channels == 3 ? i : 0;
            const float a = table[size_t(k) * channels + c], b = table[size_t(k + 1) * channels + c];
            v[i] = a + (b - a) * f;
        }
    }
};

struct Lut3DOp : ClfTransform::Op {
    int n = 0;
    bool tetrahedral = false;
    std::vector<float> table;  // n^3 x 3, blue fastest
    const float* at(int r, int g, int b) const { return &table[((size_t(r) * n + size_t(g)) * n + size_t(b)) * 3]; }
    void apply(float v[3]) const override {
        float x[3];
        int i0[3], i1[3];
        for (int c = 0; c < 3; ++c) {
            const float p = clamp01(v[c]) * float(n - 1);
            i0[c] = std::min(n - 2, int(p));
            i1[c] = i0[c] + 1;
            x[c] = p - float(i0[c]);
        }
        float out[3];
        if (!tetrahedral) {
            for (int c = 0; c < 3; ++c) {
                auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
                const float c00 = lerp(at(i0[0], i0[1], i0[2])[c], at(i1[0], i0[1], i0[2])[c], x[0]);
                const float c01 = lerp(at(i0[0], i0[1], i1[2])[c], at(i1[0], i0[1], i1[2])[c], x[0]);
                const float c10 = lerp(at(i0[0], i1[1], i0[2])[c], at(i1[0], i1[1], i0[2])[c], x[0]);
                const float c11 = lerp(at(i0[0], i1[1], i1[2])[c], at(i1[0], i1[1], i1[2])[c], x[0]);
                out[c] = lerp(lerp(c00, c10, x[1]), lerp(c01, c11, x[1]), x[2]);
            }
        } else {
            const float fr = x[0], fg = x[1], fb = x[2];
            const float* c000 = at(i0[0], i0[1], i0[2]);
            const float* c111 = at(i1[0], i1[1], i1[2]);
            for (int c = 0; c < 3; ++c) {
                float o;
                if (fr > fg) {
                    if (fg > fb) o = (1 - fr) * c000[c] + (fr - fg) * at(i1[0], i0[1], i0[2])[c] + (fg - fb) * at(i1[0], i1[1], i0[2])[c] + fb * c111[c];
                    else if (fr > fb) o = (1 - fr) * c000[c] + (fr - fb) * at(i1[0], i0[1], i0[2])[c] + (fb - fg) * at(i1[0], i0[1], i1[2])[c] + fg * c111[c];
                    else o = (1 - fb) * c000[c] + (fb - fr) * at(i0[0], i0[1], i1[2])[c] + (fr - fg) * at(i1[0], i0[1], i1[2])[c] + fg * c111[c];
                } else {
                    if (fb > fg) o = (1 - fb) * c000[c] + (fb - fg) * at(i0[0], i0[1], i1[2])[c] + (fg - fr) * at(i0[0], i1[1], i1[2])[c] + fr * c111[c];
                    else if (fb > fr) o = (1 - fg) * c000[c] + (fg - fb) * at(i0[0], i1[1], i0[2])[c] + (fb - fr) * at(i0[0], i1[1], i1[2])[c] + fr * c111[c];
                    else o = (1 - fg) * c000[c] + (fg - fr) * at(i0[0], i1[1], i0[2])[c] + (fr - fb) * at(i1[0], i1[1], i0[2])[c] + fb * c111[c];
                }
                out[c] = o;
            }
        }
        for (int c = 0; c < 3; ++c) v[c] = out[c];
    }
};

struct CdlOp : ClfTransform::Op {
    double slope[3] = {1, 1, 1}, offset[3] = {0, 0, 0}, power[3] = {1, 1, 1}, sat = 1;
    bool reverse = false, clamp = true;
    static float luma(const float v[3]) { return 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2]; }
    void saturate(float v[3], double s) const {
        const float y = luma(v);
        for (int i = 0; i < 3; ++i) v[i] = float(y + s * (v[i] - y));
    }
    void apply(float v[3]) const override {
        if (!reverse) {
            for (int i = 0; i < 3; ++i) {
                double o = v[i] * slope[i] + offset[i];
                if (clamp) o = std::clamp(o, 0.0, 1.0);
                if (o >= 0) o = std::pow(o, power[i]);  // (NoClamp: negative values pass the power unchanged)
                v[i] = float(o);
            }
            saturate(v, sat);
            if (clamp)
                for (int i = 0; i < 3; ++i) v[i] = clamp01(v[i]);
        } else {
            if (clamp)
                for (int i = 0; i < 3; ++i) v[i] = clamp01(v[i]);
            saturate(v, sat != 0 ? 1 / sat : 1);
            for (int i = 0; i < 3; ++i) {
                double o = v[i];
                if (clamp) o = std::clamp(o, 0.0, 1.0);
                if (o >= 0) o = std::pow(o, 1 / power[i]);
                o = (o - offset[i]) / slope[i];
                if (clamp) o = std::clamp(o, 0.0, 1.0);
                v[i] = float(o);
            }
        }
    }
};

struct LogParams {
    double base = 2, logSlope = 1, logOffset = 0, linSlope = 1, linOffset = 0, linBreak = NAN, linearSlope = NAN;
};

struct LogOp : ClfTransform::Op {
    enum Style { Log10, AntiLog10, Log2, AntiLog2, LinToLog, LogToLin, CameraLinToLog, CameraLogToLin } style = Log2;
    LogParams p[3];
    static double linToLog(const LogParams& q, double x) {
        return q.logSlope * std::log(std::max(q.linSlope * x + q.linOffset, double(FLT_MIN))) / std::log(q.base) + q.logOffset;
    }
    static double logToLin(const LogParams& q, double y) { return (std::pow(q.base, (y - q.logOffset) / q.logSlope) - q.linOffset) / q.linSlope; }
    static double slopeAtBreak(const LogParams& q) {
        if (std::isfinite(q.linearSlope)) return q.linearSlope;
        return q.logSlope * q.linSlope / ((q.linSlope * q.linBreak + q.linOffset) * std::log(q.base));
    }
    void apply(float v[3]) const override {
        for (int i = 0; i < 3; ++i) {
            const LogParams& q = p[i];
            const double x = v[i];
            double o = x;
            switch (style) {
                case Log10: o = std::log10(std::max(x, double(FLT_MIN))); break;
                case AntiLog10: o = std::pow(10.0, x); break;
                case Log2: o = std::log2(std::max(x, double(FLT_MIN))); break;
                case AntiLog2: o = std::exp2(x); break;
                case LinToLog: o = linToLog(q, x); break;
                case LogToLin: o = logToLin(q, x); break;
                case CameraLinToLog: {
                    const double logBreak = linToLog(q, q.linBreak);
                    o = x <= q.linBreak ? slopeAtBreak(q) * (x - q.linBreak) + logBreak : linToLog(q, x);
                    break;
                }
                case CameraLogToLin: {
                    const double logBreak = linToLog(q, q.linBreak);
                    o = x <= logBreak ? (x - logBreak) / slopeAtBreak(q) + q.linBreak : logToLin(q, x);
                    break;
                }
            }
            v[i] = float(o);
        }
    }
};

struct ExponentOp : ClfTransform::Op {
    enum Style { BasicFwd, BasicRev, BasicMirrorFwd, BasicMirrorRev, BasicPassThruFwd, BasicPassThruRev, MonCurveFwd, MonCurveRev,
                 MonCurveMirrorFwd, MonCurveMirrorRev } style = BasicFwd;
    double g[3] = {1, 1, 1}, off[3] = {0, 0, 0};
    // The linear segment's slope below the break (1/12.92 for sRGB's 2.4 and 0.055), meeting the power curve there.
    static double monSlope(double gamma, double offset) {
        return std::pow(offset * gamma / ((gamma - 1) * (1 + offset)), gamma) * (gamma - 1) / offset;
    }
    static double monFwd(double x, double gamma, double offset) {
        const double brk = offset / (gamma - 1);
        return x >= brk ? std::pow((x + offset) / (1 + offset), gamma) : x * monSlope(gamma, offset);
    }
    static double monRev(double y, double gamma, double offset) {
        const double brk = std::pow(offset * gamma / ((gamma - 1) * (1 + offset)), gamma);
        return y >= brk ? (1 + offset) * std::pow(y, 1 / gamma) - offset : y / monSlope(gamma, offset);
    }
    void apply(float v[3]) const override {
        for (int i = 0; i < 3; ++i) {
            const double x = v[i], e = g[i], o = off[i];
            double r = x;
            switch (style) {
                case BasicFwd: r = std::pow(std::max(0.0, x), e); break;
                case BasicRev: r = std::pow(std::max(0.0, x), 1 / e); break;
                case BasicMirrorFwd: r = std::copysign(std::pow(std::fabs(x), e), x); break;
                case BasicMirrorRev: r = std::copysign(std::pow(std::fabs(x), 1 / e), x); break;
                case BasicPassThruFwd: r = x < 0 ? x : std::pow(x, e); break;
                case BasicPassThruRev: r = x < 0 ? x : std::pow(x, 1 / e); break;
                case MonCurveFwd: r = monFwd(x, e, o); break;
                case MonCurveRev: r = monRev(x, e, o); break;
                case MonCurveMirrorFwd: r = std::copysign(monFwd(std::fabs(x), e, o), x); break;
                case MonCurveMirrorRev: r = std::copysign(monRev(std::fabs(x), e, o), x); break;
            }
            v[i] = float(r);
        }
    }
};

// The channel-keyed parameters (LogParams, ExponentParams): one for all three, or one per channel.
template <typename Read>
bool channelParams(const QDomElement& op, const char* name, Read read, std::string& why) {
    bool any = false;
    for (QDomElement c = op.firstChildElement(); !c.isNull(); c = c.nextSiblingElement()) {
        if (c.localName() != name && c.tagName() != name) continue;
        const QString ch = c.attribute("channel");
        int which = -1;
        if (ch == "R") which = 0;
        else if (ch == "G") which = 1;
        else if (ch == "B") which = 2;
        else if (!ch.isEmpty()) {
            why = std::string(name) + " has an unknown channel";
            return false;
        }
        for (int i = 0; i < 3; ++i)
            if (which < 0 || which == i)
                if (!read(c, i)) {
                    why = std::string(name) + " has a number that cannot be read";
                    return false;
                }
        any = true;
    }
    return any || (why = std::string(name) + " is missing").empty();
}

bool attrNumber(const QDomElement& e, const char* name, double& out) {
    if (!e.hasAttribute(name)) return true;
    bool ok = false;
    const double v = e.attribute(name).toDouble(&ok);
    if (!ok || !std::isfinite(v)) return false;
    out = v;
    return true;
}

std::shared_ptr<const ClfTransform::Op> readOp(const QDomElement& e, std::string& why) {
    const QString kind = e.localName().isEmpty() ? e.tagName() : e.localName();
    const QString inD = e.attribute("inBitDepth", "32f"), outD = e.attribute("outBitDepth", "32f");
    if (!knownDepth(inD) || !knownDepth(outD)) {
        why = kind.toStdString() + " has an unknown bit depth";
        return nullptr;
    }
    const double inS = depthScale(inD), outS = depthScale(outD);
    std::vector<double> a;
    if (kind == "Matrix") {
        const QDomElement arr = child(e, "Array");
        if (arr.isNull() || !numbers(arr.text(), a) || (a.size() != 9 && a.size() != 12)) {
            why = "A Matrix needs 9 or 12 numbers";
            return nullptr;
        }
        auto op = std::make_shared<MatrixOp>();
        const int cols = a.size() == 9 ? 3 : 4;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) op->m[r * 4 + c] = a[size_t(r * cols + c)] * inS / outS;
            op->m[r * 4 + 3] = cols == 4 ? a[size_t(r * 4 + 3)] / outS : 0;
        }
        return op;
    }
    if (kind == "Range") {
        auto op = std::make_shared<RangeOp>();
        double minIn = 0, maxIn = 1, minOut = 0, maxOut = 1;
        const bool hMinIn = numberOf(child(e, "minInValue"), minIn), hMaxIn = numberOf(child(e, "maxInValue"), maxIn);
        const bool hMinOut = numberOf(child(e, "minOutValue"), minOut), hMaxOut = numberOf(child(e, "maxOutValue"), maxOut);
        if (hMinIn != hMinOut || hMaxIn != hMaxOut || (!hMinIn && !hMaxIn)) {
            why = "A Range needs its in and out values in pairs";
            return nullptr;
        }
        minIn /= inS, maxIn /= inS, minOut /= outS, maxOut /= outS;
        op->hasMin = hMinIn, op->hasMax = hMaxIn;
        op->clamp = e.attribute("style", "clamp") != "noClamp";
        op->minOut = minOut, op->maxOut = maxOut;
        if (hMinIn && hMaxIn) {
            if (maxIn == minIn) {
                why = "A Range's in values are the same";
                return nullptr;
            }
            op->scale = (maxOut - minOut) / (maxIn - minIn);
            op->offset = minOut - minIn * op->scale;
        } else {
            op->scale = 1;  // (one end only: a shift, kept from going past it)
            op->offset = hMinIn ? minOut - minIn : maxOut - maxIn;
        }
        return op;
    }
    if (kind == "LUT1D") {
        if (e.attribute("halfDomain") == "true") {
            why = "Half-float-domain 1D LUTs are not read";
            return nullptr;
        }
        const QDomElement arr = child(e, "Array");
        const QStringList dim = arr.attribute("dim").simplified().split(' ');
        if (arr.isNull() || dim.size() != 2 || !numbers(arr.text(), a)) {
            why = "A LUT1D needs an Array of N x 1 or N x 3";
            return nullptr;
        }
        auto op = std::make_shared<Lut1DOp>();
        op->n = dim[0].toInt();
        op->channels = dim[1].toInt();
        if (op->n < 2 || (op->channels != 1 && op->channels != 3) || a.size() != size_t(op->n) * size_t(op->channels)) {
            why = "A LUT1D's Array does not match its size";
            return nullptr;
        }
        for (double v : a) op->table.push_back(float(v / outS));
        return op;
    }
    if (kind == "LUT3D") {
        const QDomElement arr = child(e, "Array");
        const QStringList dim = arr.attribute("dim").simplified().split(' ');
        if (arr.isNull() || dim.size() != 4 || !numbers(arr.text(), a)) {
            why = "A LUT3D needs an Array of N x N x N x 3";
            return nullptr;
        }
        auto op = std::make_shared<Lut3DOp>();
        op->n = dim[0].toInt();
        if (op->n < 2 || dim[1].toInt() != op->n || dim[2].toInt() != op->n || dim[3].toInt() != 3 ||
            a.size() != size_t(op->n) * op->n * op->n * 3) {
            why = "A LUT3D's Array does not match its size";
            return nullptr;
        }
        op->tetrahedral = e.attribute("interpolation") == "tetrahedral";
        for (double v : a) op->table.push_back(float(v / outS));
        return op;
    }
    if (kind == "ASC_CDL") {
        auto op = std::make_shared<CdlOp>();
        const QString style = e.attribute("style", "Fwd");
        if (style != "Fwd" && style != "Rev" && style != "FwdNoClamp" && style != "RevNoClamp") {
            why = "An ASC_CDL has an unknown style";
            return nullptr;
        }
        op->reverse = style.startsWith("Rev");
        op->clamp = !style.endsWith("NoClamp");
        const QDomElement sop = child(e, "SOPNode"), sat = child(e, "SatNode");
        auto three = [&](const char* name, double out[3]) {
            const QDomElement x = child(sop, name);
            if (x.isNull()) return true;
            if (!numbers(x.text(), a) || a.size() != 3) return false;
            std::copy_n(a.begin(), 3, out);
            return true;
        };
        if (!sop.isNull() && (!three("Slope", op->slope) || !three("Offset", op->offset) || !three("Power", op->power))) {
            why = "An ASC_CDL's SOPNode has numbers that cannot be read";
            return nullptr;
        }
        if (!sat.isNull() && !numberOf(child(sat, "Saturation"), op->sat)) {
            why = "An ASC_CDL's Saturation cannot be read";
            return nullptr;
        }
        return op;
    }
    if (kind == "Log") {
        auto op = std::make_shared<LogOp>();
        static const std::map<QString, LogOp::Style> styles{
            {"log10", LogOp::Log10},       {"antiLog10", LogOp::AntiLog10}, {"log2", LogOp::Log2},
            {"antiLog2", LogOp::AntiLog2}, {"linToLog", LogOp::LinToLog},   {"logToLin", LogOp::LogToLin},
            {"cameraLinToLog", LogOp::CameraLinToLog}, {"cameraLogToLin", LogOp::CameraLogToLin}};
        const auto st = styles.find(e.attribute("style"));
        if (st == styles.end()) {
            why = "A Log has an unknown style";
            return nullptr;
        }
        op->style = st->second;
        if (op->style >= LogOp::LinToLog) {
            if (!channelParams(e, "LogParams",
                               [&](const QDomElement& c, int i) {
                                   LogParams& q = op->p[i];
                                   return attrNumber(c, "base", q.base) && attrNumber(c, "logSideSlope", q.logSlope) &&
                                          attrNumber(c, "logSideOffset", q.logOffset) && attrNumber(c, "linSideSlope", q.linSlope) &&
                                          attrNumber(c, "linSideOffset", q.linOffset) && attrNumber(c, "linSideBreak", q.linBreak) &&
                                          attrNumber(c, "linearSlope", q.linearSlope);
                               },
                               why))
                return nullptr;
            for (const LogParams& q : op->p)
                if (q.base <= 0 || q.base == 1 || q.logSlope == 0 || q.linSlope == 0 ||
                    ((op->style == LogOp::CameraLinToLog || op->style == LogOp::CameraLogToLin) && !std::isfinite(q.linBreak))) {
                    why = "A Log's parameters cannot make a curve (a camera style needs linSideBreak)";
                    return nullptr;
                }
        }
        return op;
    }
    if (kind == "Exponent") {
        auto op = std::make_shared<ExponentOp>();
        static const std::map<QString, ExponentOp::Style> styles{
            {"basicFwd", ExponentOp::BasicFwd}, {"basicRev", ExponentOp::BasicRev},
            {"basicMirrorFwd", ExponentOp::BasicMirrorFwd}, {"basicMirrorRev", ExponentOp::BasicMirrorRev},
            {"basicPassThruFwd", ExponentOp::BasicPassThruFwd}, {"basicPassThruRev", ExponentOp::BasicPassThruRev},
            {"monCurveFwd", ExponentOp::MonCurveFwd}, {"monCurveRev", ExponentOp::MonCurveRev},
            {"monCurveMirrorFwd", ExponentOp::MonCurveMirrorFwd}, {"monCurveMirrorRev", ExponentOp::MonCurveMirrorRev}};
        const auto st = styles.find(e.attribute("style"));
        if (st == styles.end()) {
            why = "An Exponent has an unknown style";
            return nullptr;
        }
        op->style = st->second;
        if (!channelParams(e, "ExponentParams",
                           [&](const QDomElement& c, int i) { return attrNumber(c, "exponent", op->g[i]) && attrNumber(c, "offset", op->off[i]); },
                           why))
            return nullptr;
        const bool mon = op->style >= ExponentOp::MonCurveFwd;
        for (int i = 0; i < 3; ++i)
            if (op->g[i] <= 0 || (mon && (op->g[i] <= 1 || op->off[i] <= 0))) {
                why = "An Exponent's parameters cannot make a curve (a monCurve needs an exponent over 1 and an offset over 0)";
                return nullptr;
            }
        return op;
    }
    why = "\"" + kind.toStdString() + "\" is not a CLF operator Montage reads";
    return nullptr;
}

}  // namespace

void ClfTransform::apply(float rgb[3]) const {
    for (const auto& op : ops) op->apply(rgb);
}

std::shared_ptr<const ClfTransform> parseClf(const std::string& xml, std::string* error) {
    auto fail = [&](const std::string& why) -> std::shared_ptr<const ClfTransform> {
        if (error) *error = why;
        return nullptr;
    };
    QDomDocument doc;
    if (!doc.setContent(QByteArray::fromStdString(xml), true)) return fail("Not an XML file");
    const QDomElement root = doc.documentElement();
    if (root.localName() != "ProcessList" && root.tagName() != "ProcessList") return fail("Not a CLF file (no ProcessList)");
    auto t = std::make_shared<ClfTransform>();
    t->name = root.attribute("name").toStdString();
    static const QStringList skip{"Description", "InputDescriptor", "OutputDescriptor", "Info"};
    for (QDomElement e = root.firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
        const QString kind = e.localName().isEmpty() ? e.tagName() : e.localName();
        if (skip.contains(kind)) {
            if (kind == "Description" && t->description.empty()) t->description = e.text().trimmed().toStdString();
            continue;
        }
        std::string why;
        auto op = readOp(e, why);
        if (!op) return fail(why);
        t->ops.push_back(std::move(op));
    }
    return t;
}

std::shared_ptr<const ClfTransform> loadClf(const std::string& path, std::string* error) {
    const QFileInfo fi(QString::fromStdString(path));
    const std::string stamp = path + "|" + std::to_string(fi.size()) + "|" + std::to_string(fi.lastModified().toMSecsSinceEpoch());
    static std::mutex m;
    static std::map<std::string, std::shared_ptr<const ClfTransform>> cache;
    {
        std::lock_guard lock(m);
        if (auto it = cache.find(stamp); it != cache.end()) return it->second;
    }
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = "Cannot read " + path;
        return nullptr;
    }
    auto t = parseClf(f.readAll().toStdString(), error);
    if (!t) return nullptr;
    std::lock_guard lock(m);
    if (cache.size() > 32) cache.clear();
    cache[stamp] = t;
    return t;
}

}  // namespace montage
