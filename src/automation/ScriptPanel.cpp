#include "ScriptPanel.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <set>

namespace montage {

namespace {

const QStringList kTypes = {"text", "number", "check", "choice", "slider", "label", "separator", "button"};

QJsonValue number(double v) { return v == std::floor(v) && std::fabs(v) < 9.0e15 ? QJsonValue(qint64(v)) : QJsonValue(v); }

}  // namespace

const PanelControl* PanelSpec::find(const QString& id) const {
    if (id.isEmpty()) return nullptr;
    for (const PanelControl& c : controls)
        if (c.id == id) return &c;
    return nullptr;
}

std::optional<PanelSpec> parsePanel(const QJsonObject& d, QString* error) {
    auto fail = [error](const QString& e) -> std::optional<PanelSpec> {
        if (error) *error = e;
        return std::nullopt;
    };
    for (auto it = d.begin(); it != d.end(); ++it)
        if (it.key() != QLatin1String("title") && it.key() != QLatin1String("controls"))
            return fail(QStringLiteral("a panel has a title and controls, not \"%1\"").arg(it.key()));
    if (d.contains("title") && !d.value("title").isString()) return fail(QStringLiteral("a panel's title is text"));
    if (!d.value("controls").isArray() || d.value("controls").toArray().isEmpty())
        return fail(QStringLiteral("a panel needs controls = {{type = ..., id = ...}, ...}"));
    const QJsonArray list = d.value("controls").toArray();
    if (list.size() > 200) return fail(QStringLiteral("a panel has at most 200 controls"));
    PanelSpec spec;
    spec.title = d.value("title").toString().trimmed();
    if (spec.title.isEmpty()) spec.title = QStringLiteral("Panel");
    static const QRegularExpression idRule(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]{0,63}$"));
    std::set<QString> ids;
    for (qsizetype i = 0; i < list.size(); ++i) {
        const QString at = QStringLiteral("control %1").arg(i + 1);
        if (!list[i].isObject()) return fail(QStringLiteral("%1 is not a table").arg(at));
        const QJsonObject o = list[i].toObject();
        PanelControl c;
        c.type = o.value("type").toString();
        if (!kTypes.contains(c.type)) return fail(QStringLiteral("%1: type is one of %2").arg(at, kTypes.join(QStringLiteral(", "))));
        c.id = o.value("id").toString();
        const bool needsId = c.type != QLatin1String("label") && c.type != QLatin1String("separator");
        if (o.contains("id") && !o.value("id").isString()) return fail(QStringLiteral("%1: its id is text").arg(at));
        if (c.id.isEmpty() && needsId) return fail(QStringLiteral("%1 (%2) needs an id").arg(at, c.type));
        if (!c.id.isEmpty()) {
            if (!idRule.match(c.id).hasMatch()) return fail(QStringLiteral("%1: id \"%2\" is letters, digits and _ (not starting with a digit)").arg(at, c.id));
            if (c.id == QLatin1String("pressed")) return fail(QStringLiteral("%1: \"pressed\" names the button pressed, not a control").arg(at));
            if (!ids.insert(c.id).second) return fail(QStringLiteral("%1: id \"%2\" is used twice").arg(at, c.id));
        }
        for (const char* key : {"label", "text"})
            if (o.contains(key) && !o.value(key).isString()) return fail(QStringLiteral("%1: its %2 is text").arg(at, QLatin1String(key)));
        c.label = o.value("label").toString();
        c.text = o.value("text").toString();
        if (c.type == QLatin1String("button") && c.label.isEmpty()) c.label = c.id;
        for (const char* key : {"min", "max", "step"})
            if (o.contains(key) && !o.value(key).isDouble()) return fail(QStringLiteral("%1: %2 is a number").arg(at, QLatin1String(key)));
        const QJsonValue v = o.value("value");
        if (c.type == QLatin1String("text")) {
            if (!v.isUndefined() && !v.isString()) return fail(QStringLiteral("%1: a text's value is text").arg(at));
            c.value = v.toString();
        } else if (c.type == QLatin1String("check")) {
            if (!v.isUndefined() && !v.isBool()) return fail(QStringLiteral("%1: a check's value is true or false").arg(at));
            c.value = v.toBool();
        } else if (c.type == QLatin1String("choice")) {
            for (const QJsonValue& ch : o.value("choices").toArray()) {
                if (!ch.isString()) return fail(QStringLiteral("%1: choices are text").arg(at));
                c.choices << ch.toString();
            }
            if (c.choices.isEmpty()) return fail(QStringLiteral("%1: a choice needs choices = {...}").arg(at));
            if (!v.isUndefined() && !c.choices.contains(v.toString())) return fail(QStringLiteral("%1: \"%2\" is not one of its choices").arg(at, v.toString()));
            c.value = v.isUndefined() ? c.choices.front() : v.toString();
        } else if (c.type == QLatin1String("number") || c.type == QLatin1String("slider")) {
            const bool slider = c.type == QLatin1String("slider");
            c.min = o.value("min").toDouble(slider ? 0 : -1e9);
            c.max = o.value("max").toDouble(slider ? 100 : 1e9);
            c.step = o.value("step").toDouble(1);
            if (slider) {
                // (Whole numbers a slider can hold.)
                c.min = std::clamp(std::round(c.min), -1e9, 1e9), c.max = std::clamp(std::round(c.max), -1e9, 1e9);
                c.step = std::clamp(std::round(c.step), 1.0, 1e6);
            }
            if (!(c.min <= c.max) || !std::isfinite(c.min) || !std::isfinite(c.max)) return fail(QStringLiteral("%1: min is more than max").arg(at));
            if (!(c.step > 0) || !std::isfinite(c.step)) return fail(QStringLiteral("%1: step is more than 0").arg(at));
            if (!v.isUndefined() && !v.isDouble()) return fail(QStringLiteral("%1: its value is a number").arg(at));
            double x = v.isUndefined() ? std::clamp(0.0, c.min, c.max) : std::clamp(v.toDouble(), c.min, c.max);
            if (slider) x = std::round(x);
            c.value = number(x);
        } else if (!v.isUndefined()) {
            return fail(QStringLiteral("%1: a %2 has no value").arg(at, c.type));
        }
        spec.controls.push_back(c);
    }
    return spec;
}

QJsonObject panelValues(const PanelSpec& spec) {
    QJsonObject out;
    for (const PanelControl& c : spec.controls) {
        if (c.id.isEmpty() || c.type == QLatin1String("button") || c.type == QLatin1String("separator")) continue;
        out[c.id] = c.type == QLatin1String("label") ? QJsonValue(c.text) : c.value;
    }
    return out;
}

bool setPanelValue(PanelSpec& spec, const QString& id, const QJsonValue& value, QString* error) {
    auto fail = [error](const QString& e) {
        if (error) *error = e;
        return false;
    };
    PanelControl* c = nullptr;
    for (PanelControl& x : spec.controls)
        if (!id.isEmpty() && x.id == id) c = &x;
    if (!c) return fail(QStringLiteral("the panel has no control \"%1\"").arg(id));
    // Text from the command line is read as the control's kind.
    QJsonValue v = value;
    if (v.isString() && (c->type == QLatin1String("number") || c->type == QLatin1String("slider"))) {
        bool ok = false;
        const double d = v.toString().toDouble(&ok);
        if (!ok) return fail(QStringLiteral("%1 is a number, not \"%2\"").arg(id, v.toString()));
        v = d;
    } else if (v.isString() && c->type == QLatin1String("check")) {
        const QString s = v.toString().toLower();
        if (s != QLatin1String("true") && s != QLatin1String("false")) return fail(QStringLiteral("%1 is true or false, not \"%2\"").arg(id, v.toString()));
        v = s == QLatin1String("true");
    }
    if (c->type == QLatin1String("label")) {
        if (!v.isString() && !v.isDouble() && !v.isBool()) return fail(QStringLiteral("%1 is a label: its value is text").arg(id));
        c->text = v.isString() ? v.toString() : v.isBool() ? (v.toBool() ? QStringLiteral("true") : QStringLiteral("false")) : QString::number(v.toDouble(), 'g', 12);
        return true;
    }
    if (c->type == QLatin1String("button") || c->type == QLatin1String("separator")) return fail(QStringLiteral("%1 is a %2: it has no value").arg(id, c->type));
    if (c->type == QLatin1String("text")) {
        if (!v.isString()) return fail(QStringLiteral("%1 is text").arg(id));
        c->value = v;
    } else if (c->type == QLatin1String("check")) {
        if (!v.isBool()) return fail(QStringLiteral("%1 is true or false").arg(id));
        c->value = v;
    } else if (c->type == QLatin1String("choice")) {
        if (!v.isString() || !c->choices.contains(v.toString())) return fail(QStringLiteral("%1 is one of: %2").arg(id, c->choices.join(QStringLiteral(", "))));
        c->value = v;
    } else {
        if (!v.isDouble() || !std::isfinite(v.toDouble())) return fail(QStringLiteral("%1 is a number").arg(id));
        double x = std::clamp(v.toDouble(), c->min, c->max);
        if (c->type == QLatin1String("slider")) x = std::round(x);
        c->value = number(x);
    }
    return true;
}

bool samePanelLayout(const PanelSpec& a, const PanelSpec& b) {
    if (a.title != b.title || a.controls.size() != b.controls.size()) return false;
    for (size_t i = 0; i < a.controls.size(); ++i) {
        const PanelControl &x = a.controls[i], &y = b.controls[i];
        if (x.type != y.type || x.id != y.id || x.label != y.label || x.min != y.min || x.max != y.max || x.step != y.step ||
            x.choices != y.choices)
            return false;
    }
    return true;
}

QJsonObject describePanel(const PanelSpec& spec) {
    QJsonArray controls;
    for (const PanelControl& c : spec.controls) {
        QJsonObject o{{"type", c.type}};
        if (!c.id.isEmpty()) o["id"] = c.id;
        if (!c.label.isEmpty()) o["label"] = c.label;
        if (c.type == QLatin1String("label")) o["text"] = c.text;
        if (c.hasValue()) o["value"] = c.value;
        if (c.type == QLatin1String("number") || c.type == QLatin1String("slider")) {
            o["min"] = number(c.min);
            o["max"] = number(c.max);
            o["step"] = number(c.step);
        }
        if (c.type == QLatin1String("choice")) o["choices"] = QJsonArray::fromStringList(c.choices);
        controls.append(o);
    }
    return QJsonObject{{"title", spec.title}, {"controls", controls}};
}

}  // namespace montage
