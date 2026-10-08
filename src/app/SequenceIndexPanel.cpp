#include "SequenceIndexPanel.h"

#include <QCheckBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QTableWidget>
#include <QVBoxLayout>

#include "EditorState.h"
#include "Theme.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "core/History.h"

namespace montage {

SequenceIndexPanel::SequenceIndexPanel(EditorState* state, QWidget* parent) : QWidget(parent), state_(state) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    auto* top = new QHBoxLayout;
    filter_ = new QLineEdit(this);
    filter_->setObjectName(QStringLiteral("indexFilter"));
    filter_->setPlaceholderText(tr("Search clips and markers (name, colour, track, media, effects…)"));
    filter_->setClearButtonEnabled(true);
    top->addWidget(filter_, 1);
    count_ = new QLabel(this);
    count_->setObjectName(QStringLiteral("indexCount"));
    top->addWidget(count_);
    lay->addLayout(top);
    rolesBar_ = new QWidget(this);
    rolesBar_->setObjectName(QStringLiteral("indexRoles"));
    rolesBar_->setToolTip(tr("Audio roles: untick one to mute every clip with it"));
    auto* rolesLay = new QHBoxLayout(rolesBar_);
    rolesLay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(rolesBar_);
    table_ = new QTableWidget(0, Columns, this);
    table_->setObjectName(QStringLiteral("sequenceIndex"));
    table_->setHorizontalHeaderLabels(
        {tr("Name"), tr("Kind"), tr("Colour"), tr("Role"), tr("Track"), tr("Start"), tr("End"), tr("Duration"), tr("Source In"), tr("Media"), tr("Effects")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setAlternatingRowColors(true);
    table_->setSortingEnabled(true);
    lay->addWidget(table_, 1);
    connect(filter_, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    connect(table_, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem* item) {
        // As a row index among those shown.
        int shown = 0;
        for (int r = 0; r < item->row(); ++r) shown += !table_->isRowHidden(r);
        activate(shown);
    });
    connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (building_ || item->column() != Name) return;
        int shown = 0;
        for (int r = 0; r < item->row(); ++r) shown += !table_->isRowHidden(r);
        rename(shown, item->text());
    });
    connect(state_, &EditorState::projectChanged, this, &SequenceIndexPanel::rebuild);
    connect(state_, &EditorState::sequenceSwitched, this, &SequenceIndexPanel::rebuild);
    rebuild();
}

void SequenceIndexPanel::rebuild() {
    building_ = true;
    table_->setSortingEnabled(false);
    table_->setRowCount(0);
    rows_.clear();
    const Sequence* s = state_->sequence();
    if (s) {
        const Project& p = state_->project();
        auto tc = [&](FrameTime f) { return QString::fromStdString(formatTimecode(f, s->fps)); };
        auto addRow = [&](const Row& row, QStringList cells) {
            const int r = table_->rowCount();
            table_->insertRow(r);
            cells.insert(Color, row.color > 0 ? QString::fromUtf8(theme::labelName(row.color)) : QString());
            cells.insert(Role, row.role);
            for (int c = 0; c < Columns; ++c) {
                auto* item = new QTableWidgetItem(cells.value(c));
                if (c == Color && row.color > 0) item->setData(Qt::DecorationRole, theme::labelColor(row.color));
                if (c != Name) item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                if (c == Name) item->setData(Qt::UserRole, int(rows_.size()));
                table_->setItem(r, c, item);
            }
            rows_.push_back(row);
        };
        auto addTrack = [&](const montage::Track& t, const QString& label, bool video) {
            for (const Clip& c : t.clips) {
                const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
                QString name = QString::fromStdString(c.name);
                const QString media = m ? QFileInfo(QString::fromStdString(m->path)).fileName() : QString();
                if (name.isEmpty()) name = media;
                QString kind = c.isGenerator() ? (c.generator.type == "title" ? tr("Title") : tr("Generator")) : video ? tr("Video") : tr("Audio");
                if (!c.enabled) kind += tr(" (off)");
                QStringList fx;
                for (const Effect& e : c.effects)
                    if (const EffectInfo* info = findEffectInfo(e.type)) fx << QString::fromStdString(info->displayName);
                Row row;
                row.clip = c.id;
                row.start = c.start;
                row.color = c.colorLabel;
                if (!video) row.role = QString::fromStdString(c.role);
                addRow(row, {name, kind, label, tc(c.start), tc(c.end()), tc(c.duration), c.isGenerator() ? QString() : tc(FrameTime(std::llround(c.sourceIn))),
                             media, fx.join(QStringLiteral(", "))});
                // The clip's markers, where the clip shows them.
                for (size_t i = 0; i < c.markers.size(); ++i) {
                    const Marker& mk = c.markers[i];
                    const FrameTime at = c.markerFrame(mk);
                    if (at < 0) continue;
                    Row mr;
                    mr.marker = true;
                    mr.clip = c.id;
                    mr.markerIndex = int(i);
                    mr.start = at;
                    mr.color = mk.color;
                    addRow(mr, {QString::fromStdString(mk.name), tr("Clip Marker"), label, tc(at), tc(at + mk.duration), tc(mk.duration), tc(mk.t), media,
                                QString::fromStdString(mk.comment)});
                }
            }
        };
        for (size_t i = 0; i < s->videoTracks.size(); ++i) addTrack(s->videoTracks[i], QStringLiteral("V%1").arg(i + 1), true);
        for (size_t i = 0; i < s->audioTracks.size(); ++i) addTrack(s->audioTracks[i], QStringLiteral("A%1").arg(i + 1), false);
        for (size_t i = 0; i < s->markers.size(); ++i) {
            const Marker& mk = s->markers[i];
            Row row;
            row.marker = true;
            row.markerIndex = int(i);
            row.start = mk.t;
            row.color = mk.color;
            addRow(row, {QString::fromStdString(mk.name), mk.chapter ? tr("Chapter") : tr("Marker"), QString(), tc(mk.t), tc(mk.t + mk.duration), tc(mk.duration), QString(),
                         QString(), QString::fromStdString(mk.comment)});
        }
    }
    table_->setSortingEnabled(true);
    building_ = false;
    applyFilter();
    rebuildRoles();
}

void SequenceIndexPanel::rebuildRoles() {
    auto* lay = static_cast<QHBoxLayout*>(rolesBar_->layout());
    while (QLayoutItem* item = lay->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->hide();
            w->deleteLater();  // a box may be the one whose toggle brought us here
        }
        delete item;
    }
    const Sequence* s = state_->sequence();
    rolesBar_->setVisible(s && !s->audioTracks.empty());
    if (!s) return;
    lay->addWidget(new QLabel(tr("Roles:"), rolesBar_));
    for (const std::string& r : edit::sequenceRoles(*s)) {
        const QString name = QString::fromStdString(r);
        auto* box = new QCheckBox(name, rolesBar_);
        box->setObjectName(QStringLiteral("role_") + name);
        box->setChecked(!edit::roleMuted(*s, r));
        connect(box, &QCheckBox::toggled, this, [this, name](bool on) { setRoleHeard(name, on); });
        lay->addWidget(box);
    }
    lay->addStretch(1);
}

QStringList SequenceIndexPanel::roles() const {
    QStringList out;
    if (const Sequence* s = state_->sequence())
        for (const std::string& r : edit::sequenceRoles(*s)) out << QString::fromStdString(r);
    return out;
}

bool SequenceIndexPanel::roleHeard(const QString& role) const {
    const Sequence* s = state_->sequence();
    return s && !edit::roleMuted(*s, role.toStdString());
}

bool SequenceIndexPanel::setRoleHeard(const QString& role, bool heard) {
    const std::string r = role.toStdString();
    return state_->edit(heard ? tr("Unmute Role %1").arg(role) : tr("Mute Role %1").arg(role), [&](Project&, Sequence& s) {
        if (edit::roleMuted(s, r) == !heard) return false;
        edit::setRoleMuted(s, r, !heard);
        return true;
    });
}

void SequenceIndexPanel::applyFilter() {
    const QString f = filter_->text().trimmed();
    int shown = 0;
    for (int r = 0; r < table_->rowCount(); ++r) {
        bool match = f.isEmpty();
        for (int c = 0; c < Columns && !match; ++c)
            if (const QTableWidgetItem* item = table_->item(r, c)) match = item->text().contains(f, Qt::CaseInsensitive);
        table_->setRowHidden(r, !match);
        shown += match;
    }
    count_->setText(tr("%1 of %2").arg(shown).arg(table_->rowCount()));
}

void SequenceIndexPanel::setFilter(const QString& text) { filter_->setText(text); }

int SequenceIndexPanel::rowCount() const {
    int n = 0;
    for (int r = 0; r < table_->rowCount(); ++r) n += !table_->isRowHidden(r);
    return n;
}

namespace {
int tableRow(const QTableWidget* t, int shown) {
    for (int r = 0; r < t->rowCount(); ++r)
        if (!t->isRowHidden(r) && shown-- == 0) return r;
    return -1;
}
}  // namespace

QString SequenceIndexPanel::cell(int row, int column) const {
    const int r = tableRow(table_, row);
    const QTableWidgetItem* item = r >= 0 ? table_->item(r, column) : nullptr;
    return item ? item->text() : QString();
}

void SequenceIndexPanel::activate(int row) {
    const int r = tableRow(table_, row);
    if (r < 0) return;
    const Row& info = rows_[size_t(table_->item(r, Name)->data(Qt::UserRole).toInt())];
    if (!info.marker) state_->setSelection({info.clip});
    state_->setPlayhead(info.start);
}

bool SequenceIndexPanel::rename(int row, const QString& name) {
    const int r = tableRow(table_, row);
    if (r < 0) return false;
    const Row info = rows_[size_t(table_->item(r, Name)->data(Qt::UserRole).toInt())];
    const std::string text = name.toStdString();
    return state_->edit(info.marker ? tr("Rename Marker") : tr("Rename Clip"), [&](Project&, Sequence& s) {
        if (info.marker) {
            std::vector<Marker>* list = &s.markers;
            if (info.clip) {
                Clip* c = edit::clipById(s, info.clip);
                if (!c) return false;
                list = &c->markers;
            }
            if (info.markerIndex < 0 || info.markerIndex >= int(list->size()) || (*list)[size_t(info.markerIndex)].name == text) return false;
            (*list)[size_t(info.markerIndex)].name = text;
            return true;
        }
        Clip* c = edit::clipById(s, info.clip);
        if (!c || c->name == text) return false;
        c->name = text;
        return true;
    });
}

}  // namespace montage
