#include "CompareDialog.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <map>

#include "EditorState.h"
#include "Theme.h"
#include "core/EditOps.h"
#include "core/MediaLog.h"

namespace montage {

int CompareDialog::labelFor(ChangeKind k) {
    switch (k) {
        case ChangeKind::Added: return labelFromName("Forest");
        case ChangeKind::Removed: return labelFromName("Red");
        case ChangeKind::Trimmed: return labelFromName("Yellow");
        case ChangeKind::Moved: return labelFromName("Cerulean");
        case ChangeKind::Changed: return labelFromName("Violet");
    }
    return 0;
}

CompareDialog::CompareDialog(EditorState* state, Id before, QWidget* parent) : QDialog(parent), state_(state) {
    setObjectName(QStringLiteral("compareDialog"));
    const Project& p = state_->project();
    const Sequence* after = state_->sequence();
    const Sequence* old = p.findSequence(before);
    setWindowTitle(tr("Compare %1 with %2").arg(QString::fromStdString(old ? old->name : ""), QString::fromStdString(after ? after->name : "")));
    resize(820, 480);
    if (old && after) changes_ = compareSequences(p, *old, *after);
    auto* lay = new QVBoxLayout(this);
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("compareSummary"));
    std::map<ChangeKind, int> counts;
    for (const TimelineChange& c : changes_) ++counts[c.kind];
    QStringList parts;
    for (ChangeKind k : {ChangeKind::Added, ChangeKind::Removed, ChangeKind::Trimmed, ChangeKind::Moved, ChangeKind::Changed})
        if (counts[k]) parts << QStringLiteral("%1 %2").arg(counts[k]).arg(QString::fromLatin1(changeKindName(k)).toLower());
    summary_->setText(changes_.empty() ? tr("No differences: the two versions play the same clips the same way.")
                                       : tr("From %1 to %2: %3.").arg(QString::fromStdString(old ? old->name : ""),
                                                                       QString::fromStdString(after ? after->name : ""), parts.join(QStringLiteral(", "))));
    summary_->setWordWrap(true);
    lay->addWidget(summary_);
    table_ = new QTableWidget(0, Columns, this);
    table_->setObjectName(QStringLiteral("compareTable"));
    table_->setHorizontalHeaderLabels({tr("Change"), tr("Clip"), tr("Track"), tr("Timecode"), tr("Length"), tr("Details")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->setStretchLastSection(true);
    const Rational fps = after ? after->fps : Rational{30, 1};
    auto tc = [&](FrameTime f) { return QString::fromStdString(formatTimecode(f, fps)); };
    for (const TimelineChange& c : changes_) {
        const int r = table_->rowCount();
        table_->insertRow(r);
        const QString track = QStringLiteral("%1%2").arg(c.track.kind == TrackKind::Video ? "V" : "A").arg(c.track.index + 1);
        const QStringList cells{QString::fromLatin1(changeKindName(c.kind)), QString::fromStdString(c.name), track, tc(c.at), tc(c.length),
                                QString::fromStdString(c.details)};
        for (int col = 0; col < Columns; ++col) {
            auto* item = new QTableWidgetItem(cells[col]);
            if (col == Change) item->setData(Qt::DecorationRole, theme::labelColor(labelFor(c.kind)));
            table_->setItem(r, col, item);
        }
    }
    table_->resizeColumnsToContents();
    lay->addWidget(table_, 1);
    connect(table_, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem* item) { activate(item->row()); });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QPushButton* markers = buttons->addButton(tr("Add as Markers"), QDialogButtonBox::ActionRole);
    markers->setObjectName(QStringLiteral("compareAddMarkers"));
    markers->setEnabled(!changes_.empty());
    connect(markers, &QPushButton::clicked, this, [this] { addMarkers(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);
}

int CompareDialog::rowCount() const { return table_->rowCount(); }

QString CompareDialog::cell(int row, int column) const {
    const QTableWidgetItem* item = table_->item(row, column);
    return item ? item->text() : QString();
}

void CompareDialog::activate(int row) {
    if (row < 0 || row >= int(changes_.size())) return;
    const TimelineChange& c = changes_[size_t(row)];
    if (c.after) state_->setSelection({c.after});
    state_->setPlayhead(c.at);
}

int CompareDialog::addMarkers() {
    const auto changes = changes_;
    int n = 0;
    state_->edit(tr("Add Change Markers"), [&](Project&, Sequence& s) {
        for (const TimelineChange& c : changes) {
            std::string name = changeKindName(c.kind);
            if (!c.name.empty()) name += ": " + c.name;
            edit::addMarker(s, Marker{c.at, c.kind == ChangeKind::Removed ? 0 : c.length, name, c.details, labelFor(c.kind)});
            ++n;
        }
        return n > 0;
    });
    return n;
}

}  // namespace montage
