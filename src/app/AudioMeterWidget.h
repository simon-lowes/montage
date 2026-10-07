// Montage — vertical stereo peak meter with dB scale, falloff, peak hold and
// latching clip indicators.
#pragma once

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>
#include <array>

namespace montage {

class AudioMeterWidget : public QWidget {
    Q_OBJECT
public:
    explicit AudioMeterWidget(QWidget* parent = nullptr);

    void setShowScale(bool show);
    bool showScale() const { return showScale_; }
    // Thinner margins and gaps for narrow placements (e.g. inside a mixer strip).
    void setCompact(bool compact);
    bool isCompact() const { return compact_; }

    // Maps a dB value to 0..1 along the meter (-60 dB = 0, +6 dB = 1, non-linear).
    static double dbToPosition(double db);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

public slots:
    // Linear peak levels (1.0 = 0 dBFS) of the latest audio block.
    void setLevels(float left, float right);
    void resetClip();
    // Clears levels, peak holds and clip indicators.
    void reset();

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    void tick();
    int scaleWidth() const;

    struct Channel {
        double level = -200;   // displayed level, dB
        double hold = -200;    // peak-hold marker, dB
        qint64 holdSince = 0;  // ms on clock_ when the hold was set
        bool clip = false;
    };
    std::array<Channel, 2> ch_;
    bool showScale_ = true;
    bool compact_ = false;
    QTimer timer_;
    QElapsedTimer clock_;
    qint64 lastTick_ = 0;
};

}  // namespace montage
