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
#include "automation/ScriptPanel.h"
#include "TimelineWidget.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "media/Image.h"
#include "render/Shorts.h"
#include "render/ReviewExport.h"
#include "render/AafExport.h"
#include "render/Dcp.h"
#include "render/Adm.h"
#include "render/Iamf.h"
#include "render/Imf.h"
#include "render/Jpeg2000.h"
#include "render/ExtendClip.h"
#include "render/DialogueMatch.h"
#include "render/RoomTone.h"
#include "render/Versions.h"
#include "render/RemoteReview.h"
#include "render/Intelligibility.h"
#include "render/LoudnessReport.h"

class QAction;
class QActionGroup;
class QDockWidget;
class QLabel;
class QMenu;

class QTabBar;

namespace montage {

class SpectralRepairDialog;
class PanFollowDialog;
class RedactFacesDialog;
class LearnEffectDialog;

class EditorState;
class CompareDialog;
class MergeDialog;
class CleanFeedWindow;
class MonitorCalibrationDialog;
class PatchGenerator;
class ReferenceOutputController;
class RemoteReviewController;
struct ConsolidateOptions;
class PlaybackController;
class MonitorPanel;
class MediaBinWidget;
class RenderQueue;
class EffectsBrowser;
class InspectorWidget;
class GradeNodePanel;
class GalleryPanel;
class ClipStripPanel;
class TrimPanel;
class ScopesWidget;
class MixerPanel;
class ControlSurface;
class CaptionsPanel;
class TranscriptPanel;
class ShotSearchPanel;
class SoundLibraryPanel;
class PeoplePanel;
class MulticamPanel;
class AdrPanel;
class MonitoringPanel;
class CueSheetPanel;
class LinedScriptPanel;
class StackedTimelineWidget;
class ProductionPanel;
class AudioDescriptionDialog;
class AudioMeterWidget;
class LoudnessReadout;
class VoiceoverDialog;
class LinkMediaDialog;
class LiveLink;
class AssistantPanel;
class ScriptConsole;
class OscControl;
class SpeedEditorControl;
class TangentControl;
class CaptureDialog;
class ScriptPanelWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    EditorState* state() const { return state_; }
    TimelineWidget* timeline() const { return timeline_; }
    RenderQueue* renderQueue() const { return queue_; }
    bool openProject(const QString& path);
    // Opening a shared project (core/ProjectLock.h): Ask asks whether to open it read-only when someone else is
    // editing it (or to take it over when they left it locked); Edit and ReadOnly say up front.
    enum class OpenMode { Ask, Edit, ReadOnly };
    bool openProjectAs(const QString& path, OpenMode mode);
    // Productions (core/Production.h): a folder of a team's projects, shown in the Production panel; `create` makes
    // the folder a production if it is not one.
    bool openProduction(const QString& folder, bool create = false, const QString& name = QString());
    ProductionPanel* productionPanel() const { return production_; }
    // Brings sequences (by name; all when none are named) from another project into this one, with their media; one
    // undo step. Returns the new sequences' ids.
    std::vector<Id> importSequencesFrom(const QString& project, const QStringList& names = {}, QString* error = nullptr);
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
    // Clip › Match Colour Chart…: the selected clip's (or the one at the playhead's) chart matched; how many clips
    // took the match. (Tests: chartDialogHook_ gets the dialog before it runs.)
    int matchColourChart();
    std::function<void(class ChartMatchDialog*)> chartDialogHook_;
    // Clip › Correct Lens…: the selected clip's (or the one at the playhead's) lens profile chosen and its Lens
    // Correction put on (app/LensCorrectionDialog.h; tests reach the dialog through lensDialogHook()).
    bool correctLens();
    // Clip › Calibrate Lens…: a lens profile made from the clip (a checkerboard in it, or lines drawn on its frame),
    // saved and applied (app/LensCalibrationDialog.h; tests reach the dialog through lensCalibrationDialogHook()).
    bool calibrateLens();
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
    // Dialogue Match (render/DialogueMatch.h): the selected audio clips made to sound like the voice reference's clip
    // (the production take): its room, tone and level, and with `roomTone` its room tone on a track under each.
    // Returns how many were matched. The dialog asks which first.
    int matchDialogue(const DialogueMatchOptions& o, bool roomTone = false);
    void matchDialogueDialog();
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
    // Clip › Breath Control: the breaths in the selected audio clips turned down by `amountDb` with volume keys (one
    // undo step). How many breaths.
    int breathControl(double amountDb);
    // Clip › Repair Damaged Frames: the selected video clips' broken frames (else the clip under the playhead's) found
    // and listed on their Frame Repair effect, which rebuilds them, each marked on the timeline (one undo step). How many.
    int repairDamagedFrames();
    // Sequence › Dialogue Intelligibility: the active sequence's dialogue measured against `preset`'s limits (render/
    // Intelligibility.h), shown in the timeline's intelligibility lane and, at the playhead, the meters' INT readout.
    // The average, or -1 when it could not be measured.
    double analyseIntelligibility(const QString& preset);
    // Sequence › Loudness Report: the active sequence's sound in its own channels measured against `spec` (render/
    // LoudnessReport.h: integrated and dialogue loudness, ranges, true peak, Leq(M), the readings every 100 ms), shown
    // in the timeline's loudness lane (green, amber over the target, red over the ceiling) and, with `base`, written
    // as base.pdf and base.csv. Whether it was measured.
    bool loudnessReport(const LoudnessReportSpec& spec, const QString& base = {});
    // Clip › Import Embedded Captions: the CEA-608 captions inside the selected video clip's file (else the clip under
    // the playhead on the target video track) as a new caption track where the clip plays them. How many captions.
    int importEmbeddedCaptions();
    // Clip › Read Burned-In Subtitles: the subtitles in the selected video clip's picture (`where`: 0 bottom, 1 top,
    // 2 the whole picture) read off it (media/TextReader.h, "en" or another Latin-script `language`) into a new caption
    // track where the clip plays them; `ask` shows the choices first. How many captions were made.
    int readBurnedInSubtitles(int where = 0, const QString& language = QStringLiteral("en"), bool ask = true);
    // Clip › Remove Mic Bleed: on multitrack talk (a mic per speaker), each track dips while it is not its speaker's
    // turn (media/MicBleed.h), as volume keyframes, one undo step. `tracks` empty = every unmuted audio track with
    // clips. How many clips changed (0 with a message when nothing applies).
    int removeMicBleed(std::vector<int> tracks = {}, double reductionDb = -24);
    void micBleedDialog();
    // Clip › Spectral Repair…: the editor for the selected audio clip (or the one under the playhead), shown modeless.
    SpectralRepairDialog* spectralRepairDialog();
    RedactFacesDialog* redactFacesDialog();
    LearnEffectDialog* learnEffectDialog();
    PanFollowDialog* panFollowDialog();
    // Clip › Remove Letterbox: black bars baked into the selected video clips' pictures found, cropped off and the rest
    // scaled to fill the frame (render/Letterbox.h), one undo step. How many clips changed.
    int removeLetterbox();
    void watchFoldersDialog();  // File › Watch Folders… (EditorState::addWatchFolder)
    // Sequence › Analyse HDR Light Levels: an HDR sequence's MaxCLL and MaxFALL measured over In to Out (or all of it)
    // and saved with it (render/LightLevel.h), one undo step, for exports that state them up front; `ask` shows the
    // result with a way to the brightest frame. False (with a message) for SDR sequences or when cancelled.
    bool analyseHdrLightLevels(bool ask = true);
    // Trims › Analyse Trims: every shot's light measured from the HDR master over In to Out (or all of it) and a starting
    // trim for each target put on the shots that have none of their own (render/AutoTrims.h), one undo step. False
    // (with a message) for SDR masters, when there are no targets or nothing to fill, or when cancelled.
    bool analyseTrims();
    // Trims › Master Beside: the Program monitor split between the master (left) and the target shown (right).
    void setMasterBeside(bool on);
    bool masterBeside() const { return masterBeside_; }
    // Edit > Control Surface: the Mackie Control surface's MIDI ports (one dialog, shown and returned).
    QDialog* controlSurfaceDialog();
    ControlSurface* controlSurface() const { return surface_; }
    OscControl* oscControl() const { return osc_; }
    // The Speed Editor's keys (from Edit > Control Surface): what each does, any menu command.
    QDialog* speedEditorKeysDialog(QWidget* parent = nullptr);
    SpeedEditorControl* speedEditor() const { return speedEditor_; }
    TangentControl* tangent() const { return tangent_; }
    // File › Capture (made when first opened; it stays, recording, while hidden).
    CaptureDialog* captureDialog();
    // Clip › Colour Group (core/ColorGroups.h), each one undo step: a new group of the selected video clips (its id, 0
    // with a message when none are selected), the selection joining a group, or leaving its group.
    Id newColorGroup(const QString& name = {});
    // File › Export Versions (render/Versions.h): the current sequence in several shapes, the reframed copies added in
    // one undo step and every version queued to render into `folder` with the preset `presetName`. The versions' ids.
    // Queues a render of each trim pass of the sequence into `folder`; how many.
    int queueTrimPasses(const QString& folder);
    std::vector<Id> exportVersions(const std::vector<VersionShape>& shapes, const QString& folder, bool captions = true, double lufs = -14,
                                   const QString& presetName = QStringLiteral("H.264 - High Quality"));
    void exportVersionsDialog();
    // File › Export for Review (render/ReviewExport.h): the page written into `folder` and the review copy queued. The
    // page's path ("" if nothing was exported).
    QString exportForReview(const QString& folder, const ReviewExportOptions& options = {});
    // Edit › Delete Gaps (core edit::deleteGaps): the stretches empty on every track closed in one undo step. How many.
    int deleteGaps(bool leading = false);
    // Fill Gap with Room Tone (render/RoomTone.h): the gap on an audio track around `at` filled (or from `at` to `to`
    // when `to` is given, within the gap) with room tone learned from `source` (0 = the clip before the gap, or after
    // it). The new clip (0 if none).
    Id fillRoomTone(TrackRef track, FrameTime at, FrameTime to = 0, Id source = 0);
    QString newRoomTonePath(const std::string& sequenceName) const;  // a new file in the project's Room Tone folder
    // Sequence > Add LTC Track: the sequence's timecode as LTC (In to Out, else all of it) on a new muted audio track.
    Id addLtcTrack();
    // Clip › Extend Clip (render/ExtendClip.h): the video clip carried on `frames` past its end (holding its last frame
    // as its motion settles), with room tone under its linked sound, pushing what follows when `ripple`. One undo step;
    // the new clips (the picture first).
    std::vector<Id> extendClip(Id clip, FrameTime frames, bool ripple = true);
    // Clip › Key Out Green / Blue Screen: the selected clips (or the one under the playhead) keyed with the Keyer set to
    // their screen's colour, read from the picture. How many.
    int keyOutScreen();
    // Tools › Agent Link: AI agents edit the open project over MCP (app/LiveLink.h). On or off (remembered), and the
    // dialog with how to connect.
    LiveLink* liveLink() const { return liveLink_; }
    ScriptConsole* scriptConsole() const { return scripts_; }
    // Scripts' own panels (montage.panel), by key (their script's file, or "console"), and those open when Montage
    // last closed opened again (their scripts run as far as montage.panel, with no tools).
    ScriptPanelWidget* scriptPanel(const QString& key) const;
    void restoreScriptPanels();
    AssistantPanel* assistant() const { return assistant_; }
    bool setAgentLink(bool on);
    void agentLinkDialog();
    void exportForReviewDialog();
    // File › Export DCP…: the sequence as a Digital Cinema Package (render/Dcp.h), made in the background with a
    // progress bar and checked when done. The folder it made ("" if it failed or was cancelled); what the check found.
    QString exportDcpTo(const QString& parent, const DcpSettings& settings, QStringList* problems = nullptr);
    void exportDcpDialog();
    // File › Make KDMs…: keys for cinema servers to play an encrypted DCP (render/Dcp.h makeKdms), from its DKDM, for
    // the servers' certificates and a window.
    void makeKdmsDialog();
    QString exportImfTo(const QString& parent, const ImfSettings& settings, QStringList* problems = nullptr);
    void exportImfDialog();
    // File › Export Immersive Master (ADM BWF)…: the bed and the audio objects (render/Adm.h), in the background.
    bool exportAdmTo(const QString& path, const AdmSettings& settings, AdmResult* result = nullptr);
    void exportAdmDialog();
    // File › Export Immersive Audio (IAMF)…: the mix as IAMF in an MP4 or the bare stream (render/Iamf.h), in the background.
    bool exportIamfTo(const QString& path, const IamfSettings& settings, IamfResult* result = nullptr);
    void exportIamfDialog();
    bool addToColorGroup(Id group);
    bool removeFromColorGroup();
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
    // Rebuilds the LUT effect (`lutEffect`, 0: the first) of the clip under the playhead as an editable grade, with the
    // residual LUT written to `residualPath`; the grade with the LUT is kept as a grade version.
    bool rebuildLutAsGrade(const QString& residualPath, Id lutEffect = 0);
    // Measures the grain of the clip under the playhead and saves it as a Grain Match preset; the preset's file.
    QString measureGrainAsPreset();
    // Avid Log Exchange (core/Ale.h): an ALE's log fields and CDLs onto the matching media (and, with `cdlToClips`,
    // the CDLs onto their clips), as one undo step, returning how many media items matched (-1 if it cannot be read);
    // the selected media (else all) as an ALE.
    int importAle(const QString& path, bool cdlToClips = true);
    bool exportAle(const QString& path);
    // Film lists of the active sequence (render/FilmLists.h): .txt, .csv or .pdf; with `earlier` (another sequence),
    // the change list from it too.
    bool exportFilmLists(const QString& path, Id earlier = 0);
    // Delivery paperwork (render/PaperworkOutput.h): `list` is "cue_sheet", "dialogue_list", "as_broadcast" or "ccsl";
    // written as the extension says (.xlsx, .csv, .pdf; .txt for the dialogue lists), behind a progress dialog.
    bool exportPaperwork(const QString& list, const QString& path);
    void exportPaperworkDialog(const QString& list);
    // ASC CDL files (core/Cdl.h): a .cc, .ccc or .cdl onto the selected video clips (one correction onto all of them,
    // a collection matched by id to each clip's name, file or tape), returning how many clips were graded; the selected
    // clips' CDLs (else every clip with one) as a .cc (one) or a .ccc or .cdl.
    int importCdl(const QString& path);
    bool exportCdl(const QString& path);
    // ACES Metadata Files (render/Amf.h): one onto the selected clips (else the clips it names); `setSpace` -1 asks
    // whether the sequence takes its output transform's space. Export writes one for each selected clip (`target` a
    // file for one, a folder for more).
    int importAmf(const QString& path, int setSpace = -1);
    int exportAmf(const QString& target);
    // Play › Ambisonic Monitoring › the head binaural listening hears through (media/Sofa.h): "" the spherical head
    // model, "ku100" SADIE II's KU100 (downloaded when asked, after asking), or a SOFA file. False (with a message
    // unless `quiet`) when it cannot be used; the choice is then left as it was.
    bool setBinauralHrtfChoice(const QString& choice, bool quiet = false);
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
    // File › Save a Copy for Another Editor: the project as it is, saved to `path` with this version as its merge base
    // (core/ProjectMerge.h), so it can be merged back later. This project stays open. False (with a message) on failure.
    bool saveCopyForAnotherEditor(const QString& path);
    // File › Merge Project (app/MergeDialog.h), with their copy at `theirs` read when given.
    MergeDialog* mergeProjectDialog(const QString& theirs = {});
    // Video layouts (Clip › Layout): the selected video clips, or with fewer than two every video clip under the
    // playhead, sharing the frame. Returns how many clips were arranged.
    int arrangeLayout(edit::Layout layout, double gap = 0);
    int joinThroughEdits();  // the selected clips' through edits, else all of the sequence's; how many (one undo step)
    bool swapClip(bool withNext);                     // the selected clip (or the one under the playhead) with its neighbour
    bool replaceWithSource();                         // the selected clip, or the one under the playhead
    bool fitToFill();
    int selectForward(bool allTracks);
    // The keyboard's way round the timeline (each says what it selected to a screen reader): the clip before or after
    // the selected one on its track (or the playhead's on the target track), the one at the same time on the track
    // above or below (it becomes the target), the one under the playhead. Each moves the playhead to the clip's start
    // except the last; false when there is none.
    bool selectNeighbourClip(int step);
    bool selectClipOnTrack(int step);
    bool selectClipAtPlayhead();
    // F6, Shift+F6: the keyboard focus to the next or previous panel shown (the timeline counts as one), and Ctrl+F6 the
    // next panel tabbed with the focused one brought to the front. The panel focused, or null.
    QDockWidget* focusPanel(int step);
    QDockWidget* nextPanelTab();
    // Window › High Contrast and Text Size (kept between sessions).
    void setHighContrast(bool on);
    void setTextScale(double scale);
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
    // gpu::initialize, marked in the settings until the GPU has drawn a frame (or Montage quits), so a launch a driver
    // crash ends leaves GPU rendering off the next time.
    bool startGpuGuarded(std::string* why);
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
    void arrangeDocks(const DockGroups& top, const DockGroups& left, const DockGroups& right, const DockGroups& bottom = {});
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
    // Printer lights on the graded clip's Color Correct: `channel` (0 R, 1 G, 2 B, -1 all three) moved by `points`.
    void printerLight(int channel, int points);
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
    void exportAafDialog(bool picture = true);
    // The cut's media as Media Composer keeps its own (OP-Atom in "Avid MediaFiles/MXF") with an AAF linked to it.
    void exportAafWithMediaDialog();
    // Find Shots for moments that look like the selected clip (or the one under the playhead) at the playhead.
    void findSimilarShots();
public:
    bool exportAafTo(const QString& path, QString* summary = nullptr, bool picture = true, const AafAvidMedia& avid = {});
private:
    // Imports an OTIO, EDL or FCP XML timeline as a new sequence (one undo step).
    void importTimeline();
    void importImfPackage();
    void addTitle();
public:
    // Reads a timeline file, or an IMF package (a folder, its ASSETMAP.xml or a CPL), into a new sequence.
    void importTimelineFrom(QString path);
    // Adds the OGraf graphic `file` (asked for when "") at the playhead, its data its defaults; the clip, or 0.
    Id addOGrafGraphic(const QString& file = {});
private:
    void newSequence();
    void openInSource(Id media);
    Id makeSubclip();
    void applyFromBrowser(const QString& type, EffectCategory category);
    void scanPluginsInBackground();
    void offerRecovery(const std::vector<RecoveryManager::Session>& crashed);
    void openSnapshot();
    void showShortcuts();
    void showReferenceOutput();
    MonitorCalibrationDialog* showMonitorCalibration();
    void showRemoteReview();
    // Markers reviewers sent from a remote review (one undo step), on the sequence streamed.
    int applyRemoteMarkers(Id sequence, const std::vector<output::RemoteMarker>& markers);
    void about();

    EditorState* state_;
    LiveLink* liveLink_ = nullptr;
    QAction* agentLinkAction_ = nullptr;
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
    // Playback › Video Output › Reference Output…: the Program to a DeckLink card or over NDI.
    ReferenceOutputController* referenceOutput_ = nullptr;
    RemoteReviewController* remoteReview_ = nullptr;
    QAction* remoteReviewAction_ = nullptr;
    QAction* referenceOutputAction_ = nullptr;
    PatchGenerator* patchGenerator_ = nullptr;
    QAction* monitorCalibrationAction_ = nullptr;
    MixerPanel* mixer_ = nullptr;
    ControlSurface* surface_ = nullptr;
    OscControl* osc_ = nullptr;
    SpeedEditorControl* speedEditor_ = nullptr;
    TangentControl* tangent_ = nullptr;
    CaptureDialog* capture_ = nullptr;
    MulticamPanel* multicam_ = nullptr;
    AdrPanel* adr_ = nullptr;
    MonitoringPanel* monitoring_ = nullptr;
    CueSheetPanel* cueSheet_ = nullptr;
    LinedScriptPanel* linedScript_ = nullptr;
    ProductionPanel* production_ = nullptr;
    QDockWidget* productionDock_ = nullptr;
    QAction* takeEdit_ = nullptr;
    void importFromProjectDialog(QString project = QString());
    bool takeEditInteractive();  // File › Edit Project: asks before discarding changes not saved
    void newProjectInProduction(const QString& folder);
    AudioDescriptionDialog* audioDescription_ = nullptr;
    CaptionsPanel* captions_ = nullptr;
    TranscriptPanel* transcript_ = nullptr;
    ShotSearchPanel* shots_ = nullptr;
    SoundLibraryPanel* soundLibrary_ = nullptr;
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
    // Each sequence's last intelligibility analysis, stale once the project is edited after it.
    struct IntelligibilityRun {
        Intelligibility result;
        IntelligibilityLimits limits;
        uint64_t revision = 0;    // the edit history's when the sequence was copied to be measured
        uint64_t generation = 0;  // the project's (EditorState::projectGeneration): another project's runs are dropped
    };
    std::map<Id, IntelligibilityRun> intelligibility_;
    void showIntelligibility();
    // Each sequence's last loudness report, the same way.
    struct LoudnessRun {
        DialogueLoudness result;
        LoudnessReportSpec spec;
        uint64_t revision = 0, generation = 0;
    };
    std::map<Id, LoudnessRun> loudnessRuns_;
    void showLoudnessLane();
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
    QDockWidget* soundLibraryDock_ = nullptr;
    QDockWidget* peopleDock_ = nullptr;
    QDockWidget* assistantDock_ = nullptr;
    AssistantPanel* assistant_ = nullptr;
    DcpResult lastDcp_;  // the DCP this session made last (Make KDMs starts from its DKDM)
    // Tools › Scripts: Lua scripts on the open project (app/ScriptConsole.h).
    QDockWidget* scriptsDock_ = nullptr;
    ScriptConsole* scripts_ = nullptr;
    struct ScriptPanelDock {
        QDockWidget* dock = nullptr;
        ScriptPanelWidget* widget = nullptr;
        QString name, code;  // its script's name, and its text when it is the console's (a file is read afresh)
    };
    std::map<QString, ScriptPanelDock> scriptPanels_;
    void openScriptPanel(const QString& key, const QString& name, const QString& code, const PanelSpec& spec);
    void pressScriptPanel(const QString& key, const QString& button);
    QDockWidget* indexDock_ = nullptr;
    QDockWidget* stackedDock_ = nullptr;
    StackedTimelineWidget* stacked_ = nullptr;
    QDockWidget* multicamDock_ = nullptr;
    QDockWidget* adrDock_ = nullptr;
    QDockWidget* monitoringDock_ = nullptr;
    QDockWidget* cueSheetDock_ = nullptr;
    QDockWidget* linedScriptDock_ = nullptr;
    QDockWidget* queueDock_ = nullptr;
    QDockWidget* keyframesDock_ = nullptr;
    QDockWidget* nodesDock_ = nullptr;
    GradeNodePanel* gradeNodes_ = nullptr;
    QDockWidget* galleryDock_ = nullptr;
    GalleryPanel* gallery_ = nullptr;
    QDockWidget* clipsDock_ = nullptr;
    ClipStripPanel* clipStrip_ = nullptr;
    QDockWidget* trimsDock_ = nullptr;
    TrimPanel* trims_ = nullptr;
    QDockWidget* meterDock_ = nullptr;
    RenderQueue* queue_ = nullptr;
    Image colourRef_;       // the look Match Colour aims for
    QString colourRefName_;
    QImage colourRefView_;  // the reference at display size, for the compare
    std::vector<double> voiceRef_;  // Match Voice's reference spectrum
    QString voiceRefName_;
    Id voiceRefClip_ = 0;  // and its clip, for Dialogue Match (in the project of that generation)
    uint64_t voiceRefGeneration_ = 0;
    QAction* compareRef_ = nullptr;
    bool masterBeside_ = false;
    QTimer* masterTimer_ = nullptr;
    void refreshMasterBeside();
    QAction* trimView_ = nullptr;  // Playback › Two-Up Trim View
    class ReviewOverlay* reviewOverlay_ = nullptr;  // reviewers' drawings over the Program monitor
    QAction* hdrViewer_ = nullptr;  // Playback › HDR Viewer
    bool hdrMessageShown_ = false;  // why HDR is not showing, said once a toggle
    bool hdrWired_ = false;
    void applyHdrViewer();
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
