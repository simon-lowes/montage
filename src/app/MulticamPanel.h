// Montage — the Multicam panel: every angle of the multicam clip under the
// playhead side by side (up to 25, five by five). Clicking an angle (or
// pressing its key) switches to it: while the program plays, the clip is cut
// at the playhead, so a whole scene can be cut live; when stopped, the
// current shot changes (Shift cuts instead). Auto Switch cuts to whoever is
// speaking. The keys 1–9 (and the keypad's) reach a bank of nine angles: the
// bank of the angle on, unless another was picked with Alt+1–3 (or the Speed
// Editor's LIVE O/WR held with CAM 1–3).
#pragma once

#include <QFutureWatcher>
#include <QImage>
#include <QWidget>
#include <vector>

#include "core/Model.h"

class QCheckBox;
class QGridLayout;
class QLabel;
class QPushButton;
class QTimer;

namespace montage {

class EditorState;
class PlaybackController;

class MulticamPanel : public QWidget {
    Q_OBJECT
public:
    MulticamPanel(EditorState* state, PlaybackController* program, QWidget* parent = nullptr);
    ~MulticamPanel() override;

    // The multicam clip the panel works on: the selected multicam clip, else
    // the topmost one under the playhead (0 if none).
    Id currentClip() const;
    int angleCount() const { return int(views_.size()); }
    // Shows `angle` (0-based). Cuts at the playhead while playing or when `cut`.
    bool switchTo(int angle, bool cut = false);
    // Banks of nine angles for the keys: the bank they reach (0-based), how many there are, picking one (false when
    // there is no such bank), and the key's angle in it (`key` 0–8 for 1–9) switched to.
    static constexpr int kBankSize = 9;
    int bank() const;
    int bankCount() const;
    bool setBank(int bank);
    bool switchKey(int key, bool cut = false);
    // The angle images last rendered (for tests).
    std::vector<QImage> angleImages() const;
    bool audioFollowsVideo() const;
    // Opens the Auto Switch dialog for the current clip.
    void autoSwitch();

    enum class Sync { InPoints, Timecode, Audio };
    // Builds a multicam clip from media items (cameras become angles in this
    // order) as one undo step; returns its media id, or 0 (with a message).
    static Id createMulticam(EditorState* state, const std::vector<Id>& media, Sync sync, const QString& name,
                             QWidget* parent);
    // Asks for the name and how to sync, then creates the clip.
    static Id createMulticamDialog(EditorState* state, const std::vector<Id>& media, QWidget* parent);

protected:
    void showEvent(QShowEvent* e) override;

private:
    void refresh();
    void requestRender();
    void rebuildGrid(const std::vector<std::string>& names);

    EditorState* state_;
    PlaybackController* program_;
    QWidget* gridHost_ = nullptr;
    QGridLayout* grid_ = nullptr;
    std::vector<class AngleView*> views_;
    QLabel* status_ = nullptr;
    QCheckBox* audioFollows_ = nullptr;
    QPushButton* autoButton_ = nullptr;
    QTimer* throttle_ = nullptr;
    QFutureWatcher<std::vector<QImage>> watcher_;
    bool renderPending_ = false;
    Id clip_ = 0;
    int bank_ = -1;  // picked; -1: the bank of the angle on
    std::vector<std::string> names_;
};

}  // namespace montage
