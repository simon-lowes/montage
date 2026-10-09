// Montage — a layout that lines widgets up left to right and wraps them onto further rows when the room runs out
// (Qt's flow layout example), so a panel full of buttons can still be docked narrow.
#pragma once

#include <QLayout>
#include <QStyle>
#include <QList>

class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget* parent = nullptr, int spacing = -1);
    ~FlowLayout() override;

    void addItem(QLayoutItem* item) override;
    int count() const override { return int(items_.size()); }
    QLayoutItem* itemAt(int index) const override { return items_.value(index); }
    QLayoutItem* takeAt(int index) override;
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), true); }
    QSize minimumSize() const override;
    QSize sizeHint() const override;
    void setGeometry(const QRect& rect) override;

private:
    int arrange(const QRect& rect, bool measureOnly) const;
    int gap(QStyle::PixelMetric metric) const;

    QList<QLayoutItem*> items_;
    int spacing_;
};
