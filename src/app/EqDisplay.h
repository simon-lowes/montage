// Montage — the Parametric EQ's display (core/Effects.h eqBands, render/AudioFx.h ParametricEq): the EQ's response
// over frequency (20 Hz to 20 kHz on a log scale) against gain, each band's own curve faint beneath it, a dot for each
// band to drag, and the spectrum of what plays behind them. It shows and asks: the panel that holds it
// (app/EqPanel.h) makes the edits.
//
// Mouse: drag a dot to move its band's frequency (sideways) and gain (up and down), Shift for finer steps, Ctrl (Cmd on
// a Mac) to keep to whichever direction it moved most, Alt (Option) to hear only that band while dragging; the wheel
// over a dot changes its Q; double-click an empty place to add a band there, or a dot to switch its band off or on;
// right-click a dot for its type, slope, on/off and Delete.
// Keyboard: Tab and Shift+Tab move between the bands, the arrows move the band picked (Shift finer), Page Up and Page
// Down change its Q, Delete or Backspace removes it, Insert adds a band at 1 kHz, Space switches it off or on.
#pragma once

#include <QElapsedTimer>
#include <QWidget>
#include <string>
#include <vector>

#include "media/StereoAnalysis.h"

namespace montage {

class EqDisplay : public QWidget {
    Q_OBJECT
public:
    struct Band {
        std::string name;  // the effect's band ("b4", or "low" and "high")
        int type = 0;      // core/Effects.h EqBandType
        double hz = 1000, db = 0, q = 1;
        int slope = 1;     // index into eqSlopes
        bool on = true;
        bool operator==(const Band&) const = default;
    };
    explicit EqDisplay(QWidget* parent = nullptr);

    void setBands(const std::vector<Band>& bands, double outputDb);  // does not emit
    const std::vector<Band>& bands() const { return bands_; }
    int bandIndex(const std::string& name) const;  // -1 when it has none
    // The band picked (the strip below sets it, the keys move it); "" none. Does not emit.
    void setSelected(const std::string& band);
    const std::string& selected() const { return selected_; }
    // The gain shown either side of 0 dB: 6, 12, 24 or 30.
    void setRange(double db);
    double range() const { return range_; }
    // The spectrum of what plays (null: none); drawn while it keeps coming, and while showSpectrum is on.
    void setSpectrum(AudioAnalysisPtr analysis);
    void setShowSpectrum(bool on);
    bool showSpectrum() const { return showSpectrum_; }
    bool spectrumShown() const;  // drawn now
    // The whole EQ's gain at `hz` as drawn, in dB.
    double responseDb(double hz) const;
    // Where things are drawn, for tests and the panel.
    QRectF plot() const;
    double hzToX(double hz) const;
    double xToHz(double x) const;
    double dbToY(double db) const;
    double yToDb(double y) const;
    QPointF nodePos(int band) const;
    // What the hover or drag readout says ("" none).
    QString readout() const { return readout_; }
    // A band described for a screen reader and the readout: "Band 4, Bell, 1.20 kHz, +3.0 dB, Q 1.00".
    static QString describe(const Band& b);
    static QString hzText(double hz);

    QSize sizeHint() const override { return {360, 200}; }
    QSize minimumSizeHint() const override { return {200, 120}; }

signals:
    void selectionChanged(const std::string& band);
    // A dot taken (`started`), dragged and let go (`final`): the band's new frequency and gain.
    void dragStarted(const std::string& band);
    void bandMoved(const std::string& band, double hz, double db, bool final);
    // The band moved, or its Q changed, from the keyboard or the wheel (each a step).
    void bandStepped(const std::string& band, double hz, double db, double q);
    void addRequested(double hz, double db, int type);
    void removeRequested(const std::string& band);
    void typeRequested(const std::string& band, int type);
    void slopeRequested(const std::string& band, int slope);
    void onRequested(const std::string& band, bool on);
    // Alt held on a dot as it is dragged: only that band heard until it is let go.
    void soloHeld(const std::string& band, bool held);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    bool event(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void leaveEvent(QEvent* e) override;
    bool focusNextPrevChild(bool next) override;
    void focusInEvent(QFocusEvent* e) override;

private:
    int nodeAt(QPointF pos) const;
    void select(int band);  // emits
    void updateReadout(QPointF pos);
    void announce();
    // The band's colour: Montage's label colours in turn.
    static QColor colour(int band);

    std::vector<Band> bands_;
    double output_ = 0;
    std::string selected_;
    double range_ = 12;
    AudioAnalysisPtr spectrum_;
    QElapsedTimer spectrumAge_;
    bool showSpectrum_ = true;
    QString readout_;
    // A drag: the band, where it was taken and from, and what it was when Shift or Ctrl last changed.
    int drag_ = -1;
    QPointF pressAt_, anchorAt_;
    double anchorHz_ = 0, anchorDb_ = 0, pressHz_ = 0, pressDb_ = 0;
    Qt::KeyboardModifiers anchorMods_;
    bool soloing_ = false;
    bool moved_ = false;
};

}  // namespace montage
