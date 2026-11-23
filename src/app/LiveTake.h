// Montage — takes while they are recorded: the waveform of each, built from the recorder's blocks as they arrive, for
// the timeline to draw on the take's track before the take is a clip. Bucket for bucket it is the waveform the take's
// file will have (media/Decoder.h, computePeaks), so nothing moves when the clip takes its place.
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <vector>

#include "core/Model.h"
#include "media/Decoder.h"

namespace montage {

struct LiveTake {
    const QObject* owner = nullptr;  // the recorder
    Id sequence = 0;
    FrameTime at = 0;                // where the take's first sample belongs
    double fps = 30;
    int rate = 48000, channels = 1;
    // The audio tracks it is drawn on: the take's, or a grouped take's (then a channel each, in order).
    std::vector<int> tracks;
    // A loop recording (loopOut > loopIn): the take began `preroll` frames before In and goes round In to Out.
    FrameTime loopIn = 0, loopOut = 0, preroll = 0;
    qint64 frames = 0;
    std::vector<Peaks> peaks;  // one for each track drawn on

    // The pass being recorded now: its first sample in the take and the timeline frame that sample is at (the whole
    // take from where it began, when it is not a loop recording).
    void pass(qint64& first, FrameTime& from) const;
};

class LiveTakes : public QObject {
    Q_OBJECT
public:
    static LiveTakes& instance();

    // A take begins (peaks and frames are started here).
    void begin(LiveTake take);
    // The take is a loop recording: it goes round In to Out, having begun `preroll` frames before In.
    void setLoop(const QObject* owner, FrameTime in, FrameTime out, FrameTime preroll);
    // A block of it (interleaved, the take's channels).
    void add(const QObject* owner, const float* interleaved, qint64 frames);
    // The take ended or was cancelled.
    void end(const QObject* owner);

    const std::vector<LiveTake>& takes() const { return takes_; }
    const LiveTake* takeOf(const QObject* owner) const;

signals:
    void changed();

private:
    std::vector<LiveTake> takes_;
    QElapsedTimer told_;
};

}  // namespace montage
