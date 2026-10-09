// Montage — main window: panel layout, menus, shortcuts and wiring between
// the editor state, the two monitors and the timeline.
#pragma once

#include <QMainWindow>
#include <QPointer>
#include <atomic>
#include <memory>
#include <optional>
#include <QTimer>
#include <map>
#include <vector>

#include "Recovery.h"
#include "TimelineWidget.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "media/Image.h"
#include "render/Shorts.h"

class QAction;
class QActionGroup;
class QDockWidget;
class QLabel;
class QMenu;

class QTabBar;

namespace montage {

class EditorState;
class CompareDialog;
class CleanFeedWindow;
struct ConsolidateOptions;
class PlaybackController;
class MonitorPanel;
class MediaBinWidget;
class RenderQueue;
class EffectsBrowser;
class InspectorWidget;
class ScopesWidget;
class MixerPanel;
class CaptionsPanel;
class TranscriptPanel;
class ShotSearchPanel;
class PeoplePanel;
class MulticamPanel;
class AudioMeterWidget;
class LoudnessReadout;
class VoiceoverDialog;
class LinkMediaDialog;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    EditorState* state() const { return state_; }
    TimelineWidget* timeline() const { return timeline_; }
    RenderQueue* renderQueue() const { return queue_; }
    bool openProject(const QString& path);
    // Waits for pending renders and saves a screenshot of the window (testing aid).
    void scheduleScreenshot(const QString& path, int delayMs);
    // Brings a panel to the front by its object name ("inspector", "scopes", "mixer", "effects"...).
    void raisePanel(const QString& name);
    // Shot matching: remembers the Program monitor's picture at the playhead as
    // the look to match, then grades the selected clips to it (a Color Correct
    // first in each one's effects, replacing an earlier match). Returns how
    // many clips were matched.
    void setColourReference();
    bool hasColourReference() const { return !colourRef_.empty(); }
    int matchColour();
    // Split-screen compare in the Program monitor: the reference beside the current frame.
    void setCompareWithReference(bool on);
    void scriptCutDialog();  // Sequence › Build Cut from Script…
    // Music: markers on the music clip's bars or beats (returns how many), and
    // re-editing it to a length by whole bars (false if it could not).
    // Match Voice: the selected (or playing) audio clip's voice as the reference, then
    // EQ the selected audio clips to sound like it. Returns how many were matched.
    bool setVoiceReference();
    bool hasVoiceReference() const { return !voiceRef_.empty(); }
    int matchVoice();
    int addBeatMarkers(bool everyBeat);
    int cutMediaToBeat(int every, bool bars);  // the bin's selected media, cut on the music clip's beats
    Id makeHighlights(double seconds, const QString& lookFor = {});  // a new Highlights sequence; its id, or 0
    void highlightsDialog();
    // Make Shorts: the best moments of the transcribed media selected in the bin (or of every transcribed media) as
    // new sequences named "<media> - Short n", each with a render job when `queue`; their ids (none, with a message,
    // when nothing is transcribed or fits). The dialog previews the moments to pick from.
    std::vector<Id> makeShorts(const ShortsOptions& o, const ShortBuild& b, bool queue = false);
    std::vector<Id> buildShorts(const std::vector<ShortMoment>& moments, const ShortBuild& b, bool queue = false);
    std::vector<Id> shortsSource() const;
    void shortsDialog();
    int addBroll(double coverage = 0.5);  // cutaways on V2 matching what is said; how many
    bool fitMusicToLength(FrameTime target);
    void fitMusicDialog();
    // Auto Reframe: a copy of the current sequence at aspect w:h with every
    // picture clip following its subject (speed 0 slower, 1 default, 2 faster),
    // made current; 0 if cancelled or nothing to do. The dialog asks for both.
    Id autoReframeSequence(int aspectW, int aspectH, int speed);
    void autoReframeDialog();
    // Reframes the selected clips within the current sequence; returns how many.
    int autoReframeClips(int speed = 1);
    // Editing staples (each one undo step; false/0 with a message when nothing applies).
    bool rippleTrimToPlayhead(bool previous);         // Q / W
    bool pasteAttributes(unsigned what);              // edit::Attribute flags, from the copied clips
    bool removeAttributes(unsigned what);
    bool addFrameHold();                              // the clip under the playhead
    // Copies the chapter markers (within In to Out when set) as YouTube's chapter list; returns the text.
    QString copyYoutubeChapters();
    // Clip › Import Embedded Captions: the CEA-608 captions inside the selected video clip's file (else the clip under
    // the playhead on the target video track) as a new caption track where the clip plays them. How many captions.
    int importEmbeddedCaptions();
    // Clip › Remove Mic Bleed: on multitrack talk (a mic per speaker), each track dips while it is not its speaker's
    // turn (media/MicBleed.h), as volume keyframes, one undo step. `tracks` empty = every unmuted audio track with
    // clips. How many clips changed (0 with a message when nothing applies).
    int removeMicBleed(std::vector<int> tracks = {}, double reductionDb = -24);
    void micBleedDialog();
    // Clip › Remove Letterbox: black bars baked into the selected video clips' pictures found, cropped off and the rest
    // scaled to fill the frame (render/Letterbox.h), one undo step. How many clips changed.
    int removeLetterbox();
    void watchFoldersDialog();  // File › Watch Folders… (EditorState::addWatchFolder)
    // Sequence › Analyse HDR Light Levels: an HDR sequence's MaxCLL and MaxFALL measured over In to Out (or all of it)
    // and saved with it (render/LightLevel.h), one undo step, for exports that state them up front; `ask` shows the
    // result with a way to the brightest frame. False (with a message) for SDR sequences or when cancelled.
    bool analyseHdrLightLevels(bool ask = true);
    // Suggest Chapters: chapter markers where the talk moves on, titled with what each part is about (replacing the
    // chapter markers there were); `ask` shows them first to rename, drop or re-space. How many were added.
    int suggestChapterMarkers(double minSeconds = 30, bool ask = true);
    // The sequence's markers as a CSV (.csv), Avid locators (.txt) or a Resolve marker EDL (.edl); markers from a CSV
    // or Avid locator file, as one undo step (returns how many).
    bool exportMarkers(const QString& path);
    bool exportMarkersPdf(const QString& path);  // with a picture of each marker's frame
    int importMarkers(const QString& path);
    // Bakes the colour effects of the clip a command acts on, at the playhead, into a .cube file.
    bool exportClipLut(const QString& path, int size = 33);
    // File › Link Media: the dialog for the offline media (shown by itself when a project opens with some), or a
    // message and nullptr when nothing is offline.
    LinkMediaDialog* showLinkMedia();
    // File › Project Manager: copies the project and its media (render/ProjectManager.h), with progress.
    bool runProjectManager(const ConsolidateOptions& o);
    // Trim mode (Avid's and Premiere's keyboard trimming): the edit point nearest the playhead on the target video
    // track is selected and trimmed a frame (or five) at a time on its outgoing side, its incoming side, or both (a roll).
    struct TrimEdit {
        TrackRef track;
        Id outgoing = 0, incoming = 0;  // the clips either side (0 = a gap)
        int side = 0;                   // 0 both (roll), 1 outgoing (A side), 2 incoming (B side)
    };
    bool selectNearestEdit();
    void cycleTrimSide();
    bool trimSelectedEdit(FrameTime delta);
    // Extend Edit (E): the selected edit (Trim mode), else the selected clips' nearest edges, else the edit nearest
    // the playhead on the target video track, moved to the playhead; one undo step.
    bool extendEdit();
    void endTrimMode();
    const std::optional<TrimEdit>& trimEdit() const { return trimEdit_; }
    // Dynamic trimming (Avid's dynamic trim, Resolve's dynamic trim mode, Premiere's JKL in Trim mode): with an edit
    // selected, L plays its side forward through the edit and J back (again for faster, the other way at 1x), the
    // edit following live; K stops and keeps the trim as one undo step (Esc puts it back). The shuttle's clock is a
    // timer while it runs; advanceTrimShuttle moves it by hand.
    bool shuttleTrim(int direction);
    bool stopTrimShuttle(bool keep = true);
    void advanceTrimShuttle(double seconds);
    bool trimShuttling() const { return trimShuttle_.has_value(); }
    FrameTime trimShuttleFrames() const { return trimShuttle_ ? trimShuttle_->applied : 0; }
    int trimShuttleSpeed() const { return trimShuttle_ ? trimShuttle_->speed : 0; }
    void setManualTrimShuttle(bool on) { trimShuttleManual_ = on; }  // no timer: only advanceTrimShuttle moves it
    // Auditions (Final Cut's auditions, Resolve's take selector): the bin's selected media (else the Source monitor's,
    // from its In) added as takes of the selected clip (else the video clip under the playhead); then the next or
    // previous take tried in its place, or the pick kept. Each one undo step.
    int addTakesFromBin();  // how many takes were added
    bool cycleTake(int step);
    bool finalizeAudition();
    // Effect presets (core/EffectPresets.h, app/EffectPresetStore.h): the clip's effects saved under a name (the file,
    // or empty with a message), and a preset's effects added to the selected clips of its kind (how many clips).
    QString saveEffectsAsPreset(const QString& name);
    int applyEffectPreset(const QString& file);
    // Close Up (Resolve's Cut page): a punched-in copy (zoom times) of the video clip under the playhead, over In to
    // Out or the whole clip, on the track above, framed on the face found in it (else the middle). Its id, or 0.
    Id closeUp(double zoom = 1.5);
    // Audio roles: the selected clips' (and their linked audio's) role, "" for none; listening for the roles of the
    // selected clips, or of every audio clip without one. Return how many clips changed.
    int setSelectedRole(const QString& role);
    int detectRoles();
    // Reverse Match Frame: the Source monitor's frame, found in the sequence (again: the next use).
    bool reverseMatchFrame();
    // The status bar's selection readout: "3 clips selected · 00:00:12:05", the first start to the last end.
    QString selectionSummary() const;
    // Sequence › Compare with Sequence: the differences from an earlier version to this sequence, in a dialog.
    CompareDialog* compareWith(Id before);
    // Video layouts (Clip › Layout): the selected video clips, or with fewer than two every video clip under the
    // playhead, sharing the frame. Returns how many clips were arranged.
    int arrangeLayout(edit::Layout layout, double gap = 0);
    int joinThroughEdits();  // the selected clips' through edits, else all of the sequence's; how many (one undo step)
    bool swapClip(bool withNext);                     // the selected clip (or the one under the playhead) with its neighbour
    bool replaceWithSource();                         // the selected clip, or the one under the playhead
    bool fitToFill();
    int selectForward(bool allTracks);
    // Render cache: renders the frames between In and Out (the whole sequence
    // without marks) as the Program monitor shows them; returns how many were
    // rendered (-1 if cancelled). The render bar follows edits by itself;
    // refreshRenderBar(true) brings it up to date before returning.
    int renderInToOut();
    void deleteRenderFiles();
    void refreshRenderBar(bool wait = false);
    // Background render (Final Cut's): after a few quiet seconds, the stretches with effects, titles or transitions
    // are rendered into the render cache off the UI thread; editing or playing stops it until the next quiet spell.
    void setBackgroundRender(bool on);
    bool backgroundRender() const { return backgroundRender_; }
    void setBackgroundRenderDelay(int ms);
    bool backgroundRendering() const { return backgroundBusy_; }
    // Workspaces (Premiere's workspaces, Resolve's pages): the panels laid out for a task. Built in: Editing, Colour,
    // Audio, Effects, Captions and Logging (Alt+Shift+1 to 6, Window › Workspaces, or the bar in the status bar). A
    // layout of one's own is saved under a name and comes back as it was saved; the current workspace is remembered.
    static const QStringList& builtInWorkspaces();
    QStringList workspaces() const;  // the built-in ones, then saved ones by name
    QString currentWorkspace() const { return workspace_; }
    bool applyWorkspace(const QString& name);
    bool saveWorkspace(const QString& name);    // false for an empty name, a built-in's or one with a slash
    bool deleteWorkspace(const QString& name);  // saved ones only
    // Video output (Premiere's Mercury Transmit, Resolve's Video Clean Feed): the Program picture alone, full screen
    // on screen `screen` (QGuiApplication::screens(), -1 = the window's own), following playback; Esc closes it.
    CleanFeedWindow* showCleanFeed(int screen = -1);
    void hideCleanFeed();
    CleanFeedWindow* cleanFeed() const { return cleanFeed_; }
    // Animate to Audio on the selected picture clips: `param` of effect `effect` (0 = Transform) follows audio track
    // `track`'s level (0 = all) in a band (render/AudioReactive.h AudioBand), from `low` to `high`; one undo step.
    bool animateSelectionToAudio(Id effect, const std::string& param, int track, int band, double low, double high);
    // Audio channels (core/AudioChannels.h) of the selected clips' sound: play `channels` (0-based; empty = the mix),
    // or split each into a clip per channel (per pair with `pairs`); one undo step each.
    bool setSelectionChannels(const std::vector<int>& channels);
    // Align in Frame (render/ClipPlacement.h): the selected picture clips lined up with the frame inside the
    // action-safe margin ("center", "top_left"...); one undo step.
    bool alignSelection(const std::string& where);
    bool splitSelectionChannels(bool pairs);
    // Apply Default Transitions to Selection: both ends of every selected clip (of one kind with `only`); one undo step.
    void addTransitionsToSelection(std::optional<TrackKind> only = std::nullopt);
    // Speed / Duration on the selected clips (each link group once), with Maintain Audio Pitch; one undo step.
    bool setSelectionSpeed(double speed, bool maintainPitch);

protected:
    void closeEvent(QCloseEvent* e) override;
    // Dock tab bars: whole names, with scroll arrows when crowded (Qt shortens them to "Tra…" by default).
    void childEvent(QChildEvent* e) override;

private:
    enum class Monitor { Source, Program };

    void buildPanels();
    void buildActions();
    void buildMenus();
    void restoreLayout();
    void resetLayout();  // the current workspace as it was saved (a built-in one as it comes)
    // Lays the panels out afresh: groups across the top, down the left and down the right, each group's panels as tabs
    // with its first in front. Panels in no group are hidden, tabbed behind the first top group for Window to bring back.
    using DockGroups = std::vector<std::vector<QDockWidget*>>;
    void arrangeDocks(const DockGroups& top, const DockGroups& left, const DockGroups& right);
    void layOutBuiltIn(const QString& name);
    void syncWorkspaceUi();
    void syncProgram();
    void syncSequenceTabs();
    void rebuildSourceProject();
    void updateTitle();
    void updateActions();
    PlaybackController* activeController() const;

    // Commands.
    bool maybeSave();
    void newProject();
    void openDialog();
    bool save();
    bool saveAs();
    void addRecent(const QString& path);
    void rebuildRecentMenu();
    void exportMedia();
    void exportFrame();
    void copySelection(bool cut);
    void paste(bool insertMode);
    void deleteSelection(bool ripple);
    // Clear Solo (Resolve 21.1): clears every solo, remembering them; with none soloed, restores them.
    void clearOrRestoreSolo();
    // A speed ramp preset (edit::speedRampPresets, or "none") on the selected clips, one undo step.
    void speedRamp(const std::string& preset, const QString& name);
    void addEdit(bool allTracks);
    void addDefaultTransition(bool audio);
    void speedDialog();
    void vfxPullDialog();
    void animateToAudioDialog();
    void audioChannelsDialog();
    void nudge(int frames);
    void matchFrame();
    void addMarker();
    void addChapterMarker();
    void addClipMarker();
    void jumpMarker(bool forward);
    void markClip();
    void detectScenes();
    void normalizeLoudness();
    void autoColor();
    // Asks which attributes (Paste Attributes / Remove Attributes); 0 if cancelled.
    unsigned askAttributes(const QString& title, bool removing);
    // The video clip a clip command acts on: the selected one under the playhead, else the
    // top one under the playhead on the targeted track (or any).
    const Clip* clipForCommand() const;
    // The music clip a music command acts on: the selected audio clip, else the one under the playhead.
    const Clip* musicClip() const;
    void syncByAudio();
    enum class Interchange { Edl, Otio, Fcp7Xml, FcpXml };
    void exportInterchange(Interchange format);
    // The audio tracks as AAF with mono WAVs beside it (for Pro Tools, Fairlight), in the background with progress.
    void exportAafDialog();
    // Find Shots for moments that look like the selected clip (or the one under the playhead) at the playhead.
    void findSimilarShots();
public:
    bool exportAafTo(const QString& path, QString* summary = nullptr);
private:
    // Imports an OTIO, EDL or FCP XML timeline as a new sequence (one undo step).
    void importTimeline();
    void addTitle();
    void newSequence();
    void openInSource(Id media);
    Id makeSubclip();
    void applyFromBrowser(const QString& type, EffectCategory category);
    void scanPluginsInBackground();
    void offerRecovery(const std::vector<RecoveryManager::Session>& crashed);
    void openSnapshot();
    void showShortcuts();
    void about();

    EditorState* state_;
    PlaybackController* program_;
    PlaybackController* source_;
    TimelineWidget* timeline_ = nullptr;
    // Open sequences as tabs above the timeline (Premiere's and Resolve's timeline tabs).
    QTabBar* sequenceTabs_ = nullptr;
    std::optional<TrimEdit> trimEdit_;
    struct TrimShuttle {
        int speed = 0;        // frames per frame of playback, signed
        double played = 0;    // frames played since it started
        FrameTime applied = 0;  // the trim shown (stops at the media's end)
    };
    std::optional<TrimShuttle> trimShuttle_;
    QTimer* trimShuttleTimer_ = nullptr;
    qint64 trimShuttleLast_ = 0;
    bool trimShuttleManual_ = false;
    void showTrimEdit();
    std::vector<Id> openSequences_;
    MonitorPanel* sourcePanel_ = nullptr;
    MonitorPanel* programPanel_ = nullptr;
    MediaBinWidget* bin_ = nullptr;
    EffectsBrowser* effects_ = nullptr;
    InspectorWidget* inspector_ = nullptr;
    ScopesWidget* scopes_ = nullptr;
    CleanFeedWindow* cleanFeed_ = nullptr;
    MixerPanel* mixer_ = nullptr;
    MulticamPanel* multicam_ = nullptr;
    CaptionsPanel* captions_ = nullptr;
    TranscriptPanel* transcript_ = nullptr;
    ShotSearchPanel* shots_ = nullptr;
    PeoplePanel* people_ = nullptr;
    AudioMeterWidget* meter_ = nullptr;
    LoudnessReadout* loudness_ = nullptr;
    VoiceoverDialog* voiceover_ = nullptr;
    QPointer<LinkMediaDialog> linkMedia_;
    QTimer renderBarTimer_;
    QTimer backgroundTimer_;
    bool backgroundRender_ = false, backgroundBusy_ = false;
    std::shared_ptr<std::atomic<bool>> backgroundCancel_;
    void startBackgroundRender();
    void stopBackgroundRender();  // cancels a running job and waits for the next quiet spell
    int renderBarGeneration_ = 0;
    std::vector<QDockWidget*> docks_;
    QDockWidget* sourceDock_ = nullptr;
    QDockWidget* programDock_ = nullptr;
    QDockWidget* inspectorDock_ = nullptr;
    QDockWidget* binDock_ = nullptr;
    QDockWidget* effectsDock_ = nullptr;
    QDockWidget* scopesDock_ = nullptr;
    QDockWidget* mixerDock_ = nullptr;
    QDockWidget* captionsDock_ = nullptr;
    QDockWidget* transcriptDock_ = nullptr;
    QDockWidget* shotsDock_ = nullptr;
    QDockWidget* peopleDock_ = nullptr;
    QDockWidget* indexDock_ = nullptr;
    QDockWidget* multicamDock_ = nullptr;
    QDockWidget* queueDock_ = nullptr;
    QDockWidget* keyframesDock_ = nullptr;
    QDockWidget* meterDock_ = nullptr;
    RenderQueue* queue_ = nullptr;
    Image colourRef_;       // the look Match Colour aims for
    QString colourRefName_;
    QImage colourRefView_;  // the reference at display size, for the compare
    std::vector<double> voiceRef_;  // Match Voice's reference spectrum
    QString voiceRefName_;
    QAction* compareRef_ = nullptr;
    QAction* trimView_ = nullptr;  // Playback › Two-Up Trim View
    QMenu* recentMenu_ = nullptr;
    QMenu* windowMenu_ = nullptr;
    QString workspace_ = QStringLiteral("Editing");
    QTabBar* workspaceBar_ = nullptr;
    QMenu* roleMenu_ = nullptr;
    QLabel* selectionInfo_ = nullptr;
    QMenu* workspaceMenu_ = nullptr;
    QMenu* deleteWorkspaceMenu_ = nullptr;
    QAction* workspaceCustomEnd_ = nullptr;  // saved workspaces are listed before this separator
    std::vector<QAction*> workspaceActions_;
    QActionGroup* workspaceGroup_ = nullptr;
    bool syncingWorkspace_ = false;
    QLabel* statusInfo_ = nullptr;
    Monitor active_ = Monitor::Program;
    std::vector<edit::ClipboardItem> clipboard_;
    // A copied gap (Resolve 21.1): its length and track, pasted as empty space when it was copied last.
    FrameTime gapClipboard_ = 0;
    TrackRef gapTrack_;
    bool gapCopiedLast_ = false;
    std::map<Id, std::vector<Id>> soloMemory_;  // per sequence: the tracks Clear Solo un-soloed
    Id sourceSequence_ = 0;
    RecoveryManager* recovery_ = nullptr;
    QTimer syncTimer_;

    // Actions that change enabled state.
    QAction* undo_ = nullptr;
    QAction* redo_ = nullptr;
    QAction* snapping_ = nullptr;
    std::vector<std::pair<TimelineWidget::Tool, QAction*>> toolActions_;
};

}  // namespace montage
