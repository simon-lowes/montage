#include "ScriptPanelWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QVBoxLayout>
#include <cmath>

#include "Theme.h"

namespace montage {

namespace {

// Decimals enough for a number control's step (and its value).
int decimalsFor(double step) {
    int d = 0;
    while (d < 6 && std::fabs(step * std::pow(10.0, d) - std::round(step * std::pow(10.0, d))) > 1e-9) ++d;
    return d;
}

QJsonValue number(double v) { return v == std::floor(v) && std::fabs(v) < 9.0e15 ? QJsonValue(qint64(v)) : QJsonValue(v); }

}  // namespace

ScriptPanelWidget::ScriptPanelWidget(const PanelSpec& spec, QWidget* parent) : QWidget(parent), spec_(spec) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* body = new QWidget(scroll);
    auto* form = new QFormLayout(body);
    form->setContentsMargins(8, 8, 8, 8);
    for (const PanelControl& c : spec_.controls) {
        QWidget* w = nullptr;
        if (c.type == QLatin1String("text")) {
            auto* e = new QLineEdit(body);
            w = e;
        } else if (c.type == QLatin1String("number")) {
            auto* n = new QDoubleSpinBox(body);
            n->setDecimals(std::max(decimalsFor(c.step), decimalsFor(c.value.toDouble())));
            n->setRange(c.min, c.max);
            n->setSingleStep(c.step);
            w = n;
        } else if (c.type == QLatin1String("check")) {
            w = new QCheckBox(c.label, body);
        } else if (c.type == QLatin1String("choice")) {
            auto* box = new QComboBox(body);
            box->addItems(c.choices);
            w = box;
        } else if (c.type == QLatin1String("slider")) {
            auto* row = new QWidget(body);
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            auto* s = new QSlider(Qt::Horizontal, row);
            s->setRange(int(c.min), int(c.max));
            s->setSingleStep(int(c.step));
            s->setPageStep(int(c.step) * 10);
            auto* shown = new QLabel(row);
            shown->setMinimumWidth(shown->fontMetrics().horizontalAdvance(QStringLiteral("-00000")));
            connect(s, &QSlider::valueChanged, shown, [shown](int v) { shown->setNum(v); });
            h->addWidget(s, 1);
            h->addWidget(shown);
            s->setObjectName(QStringLiteral("slider"));
            w = row;
        } else if (c.type == QLatin1String("label")) {
            auto* l = new QLabel(body);
            l->setWordWrap(true);
            l->setTextFormat(Qt::PlainText);
            l->setTextInteractionFlags(Qt::TextSelectableByMouse);
            l->setText(c.text);
            w = l;
        } else if (c.type == QLatin1String("separator")) {
            auto* line = new QFrame(body);
            line->setFrameShape(QFrame::HLine);
            line->setFrameShadow(QFrame::Sunken);
            w = line;
        } else if (c.type == QLatin1String("button")) {
            auto* b = new QPushButton(c.label, body);
            connect(b, &QPushButton::clicked, this, [this, id = c.id] { emit pressed(id); });
            w = b;
        }
        if (!w) continue;
        if (!c.id.isEmpty()) {
            w->setObjectName(QStringLiteral("panel_") + c.id);
            widgets_[c.id] = w;
        }
        // A label beside the control (a check and a button carry theirs; a label or line spans the row).
        const bool spans = c.type == QLatin1String("check") || c.type == QLatin1String("button") || c.type == QLatin1String("label") ||
                           c.type == QLatin1String("separator") || c.label.isEmpty();
        if (spans) form->addRow(w);
        else form->addRow(c.label + QLatin1Char(':'), w);
        show(c);
    }
    scroll->setWidget(body);
    outer->addWidget(scroll, 1);
    message_ = new QLabel(this);
    message_->setObjectName(QStringLiteral("panelMessage"));
    message_->setWordWrap(true);
    message_->setTextFormat(Qt::PlainText);
    message_->setContentsMargins(8, 2, 8, 6);
    message_->hide();
    outer->addWidget(message_);
}

QWidget* ScriptPanelWidget::control(const QString& id) const {
    const auto it = widgets_.find(id);
    return it == widgets_.end() ? nullptr : it->second;
}

void ScriptPanelWidget::show(const PanelControl& c) {
    QWidget* w = control(c.id);
    if (!w) return;
    if (auto* e = qobject_cast<QLineEdit*>(w)) e->setText(c.value.toString());
    else if (auto* n = qobject_cast<QDoubleSpinBox*>(w)) n->setValue(c.value.toDouble());
    else if (auto* b = qobject_cast<QCheckBox*>(w)) b->setChecked(c.value.toBool());
    else if (auto* box = qobject_cast<QComboBox*>(w)) box->setCurrentText(c.value.toString());
    else if (auto* l = qobject_cast<QLabel*>(w)) l->setText(c.text);
    else if (auto* s = w->findChild<QSlider*>()) s->setValue(c.value.toInt()), emit s->valueChanged(s->value());
}

QJsonObject ScriptPanelWidget::values(const QString& pressed) const {
    QJsonObject out;
    for (const PanelControl& c : spec_.controls) {
        QWidget* w = control(c.id);
        if (!w || !c.hasValue()) {
            if (w && c.type == QLatin1String("label")) out[c.id] = qobject_cast<QLabel*>(w)->text();
            continue;
        }
        if (auto* e = qobject_cast<QLineEdit*>(w)) out[c.id] = e->text();
        else if (auto* n = qobject_cast<QDoubleSpinBox*>(w)) out[c.id] = number(n->value());
        else if (auto* b = qobject_cast<QCheckBox*>(w)) out[c.id] = b->isChecked();
        else if (auto* box = qobject_cast<QComboBox*>(w)) out[c.id] = box->currentText();
        else if (auto* s = w->findChild<QSlider*>()) out[c.id] = s->value();
    }
    out[QStringLiteral("pressed")] = pressed;
    return out;
}

bool ScriptPanelWidget::set(const QJsonObject& values, QString* error) {
    // Checked as a whole first (on a copy holding what the panel shows now), so a wrong value changes nothing.
    PanelSpec next = spec_;
    const QJsonObject now = this->values();
    for (PanelControl& c : next.controls)
        if (now.contains(c.id)) {
            if (c.type == QLatin1String("label")) c.text = now.value(c.id).toString();
            else if (c.hasValue()) c.value = now.value(c.id);
        }
    for (auto it = values.begin(); it != values.end(); ++it)
        if (!setPanelValue(next, it.key(), it.value(), error)) return false;
    for (auto it = values.begin(); it != values.end(); ++it)
        if (const PanelControl* c = next.find(it.key())) show(*c);
    spec_ = std::move(next);
    return true;
}

void ScriptPanelWidget::setBusy(bool busy) {
    for (QPushButton* b : findChildren<QPushButton*>()) b->setEnabled(!busy);
}

void ScriptPanelWidget::showMessage(const QString& text, bool error) {
    message_->setVisible(!text.isEmpty());
    message_->setText(text);
    message_->setStyleSheet(error ? QStringLiteral("color: %1;").arg(theme::kMeterRed.name()) : QString());
}

}  // namespace montage
