// Montage — the pitch of audio clips as the timeline draws it over their waveforms (audio/ElasticAudio.h): each clip's
// sound as played before its pitch edits (warped, cleaned), tracked in the background once and kept, and the pitch
// its edits ask for.
#pragma once

#include <QObject>
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audio/ElasticAudio.h"
#include "core/Model.h"

namespace montage {

class PitchCurves : public QObject {
    Q_OBJECT
public:
    struct Curve {
        PitchTrack track;            // source seconds of the clip's sound
        std::vector<double> edited;  // frame by frame of `track`: what the clip's pitch edits ask for
    };
    struct Job;
    // Clips longer than this (seconds) are not tracked for the timeline.
    static constexpr double kLongest = 900;
    explicit PitchCurves(QObject* parent = nullptr);
    ~PitchCurves() override;
    // The clip's pitch, or null while it is being worked out (ready() follows; one clip at a time, the latest asked
    // for first). With `start` false, only one already worked out (while a marker is dragged, say).
    std::shared_ptr<const Curve> curve(const Project& p, const Sequence& s, const Clip& c, bool start = true);
    // Worked out now (for tests and scripts): the same, waiting for it.
    std::shared_ptr<const Curve> curveNow(const Project& p, const Sequence& s, const Clip& c);

signals:
    void ready();

private:
    std::string keyOf(const Project& p, const Sequence& s, const Clip& c) const;
    std::shared_ptr<const Curve> finish(const std::string& key, const std::shared_ptr<const PitchTrack>& track, const Clip& c);
    void startNext();
    std::map<std::string, std::shared_ptr<const PitchTrack>> tracks_;
    std::map<std::string, std::shared_ptr<const Curve>> curves_;  // by track key and the edits
    std::deque<std::pair<std::string, std::shared_ptr<Job>>> pending_;  // newest last
    std::string running_;
    std::thread worker_;
    std::shared_ptr<std::atomic<bool>> cancel_ = std::make_shared<std::atomic<bool>>(false);
};

}  // namespace montage
