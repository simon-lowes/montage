// Montage — the Spectral Repair editor (Audition's Spectral Frequency Display, iZotope RX's spectrogram): an audio
// clip's sound as a spectrogram (time across, frequency up on a log scale, level as colour), where a box drawn
// around an unwanted sound is healed or turned down (audio/SpectralRepair.h). Each change is one undo step on the
// clip's Spectral Repair effect, and the picture shows the repaired sound.
#pragma once

#include <QDialog>
#include <QImage>
#include <QRectF>
#include <QWidget>
#include <vector>

#include "audio/SpectralRepair.h"
#include "core/Model.h"

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;

namespace montage {

class EditorState;

class SpectrogramView : public QWidget {
    Q_OBJECT
public:
    explicit SpectrogramView(QWidget* parent = nullptr);
    void setSpectrogram(const Spectrogram& g);
    void setRegions(const std::vector<SpectralRegion>& regions) {
        regions_ = regions;
        update();
    }
    // The box chosen, in source seconds and Hz (empty: none).
    bool hasSelection() const { return selEnd_ > selStart_; }
    double selectionStart() const { return selStart_; }
    double selectionEnd() const { return selEnd_; }
    double selectionLow() const { return selLow_; }
    double selectionHigh() const { return selHigh_; }
    void setSelection(double start, double end, double low, double high);
    const QImage& picture() const { return image_; }
    double minHz() const { return minHz_; }
    QSize sizeHint() const override { return {900, 360}; }

    double timeAt(double x) const;
    double hzAt(double y) const;
    double xOf(double t) const;
    double yOf(double hz) const;

signals:
    void selectionChanged();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    QRectF plot() const;
    Spectrogram g_;
    QImage image_;
    double minHz_ = 40;
    std::vector<SpectralRegion> regions_;
    double selStart_ = 0, selEnd_ = 0, selLow_ = 0, selHigh_ = 0;
    QPointF anchor_;
    bool dragging_ = false;
};

class SpectralRepairDialog : public QDialog {
    Q_OBJECT
public:
    SpectralRepairDialog(EditorState* state, Id clipId, QWidget* parent = nullptr);

    SpectrogramView* view() const { return view_; }
    // The box: source seconds and Hz (high 0 or "All frequencies": the whole range).
    void setSelection(double start, double end, double low, double high);
    void setAllFrequencies(bool all);
    bool findFrequencies();  // the box's frequencies become the band that stands out most in its time
    bool heal();
    bool attenuate(double db);
    bool removeLast();
    bool clearAll();
    std::vector<SpectralRegion> regions() const;
    QString status() const;

private:
    const Clip* clip() const;
    bool apply(const QString& label, const std::vector<SpectralRegion>& regions);
    bool add(const std::string& mode, double db);
    void refresh();  // the picture of the repaired sound and the boxes
    void selectionText();

    EditorState* state_;
    Id clip_;
    AudioBufferPtr source_;
    std::vector<SpectralRegion> shown_;  // the repairs the picture shows
    double start_ = 0, end_ = 0;  // source seconds shown
    SpectrogramView* view_;
    QCheckBox* all_;
    QDoubleSpinBox* gain_;
    QLabel* info_;
    QPushButton *heal_, *attenuate_, *find_, *undoLast_, *clear_;
};

}  // namespace montage
