// Montage — the pitch of audio clips as the timeline draws it over their waveforms (audio/ElasticAudio.h): each clip's
// sound as played before its pitch edits (warped, cleaned), tracked in the background once and kept, and the pitch
// its edits ask for.
#pragma once

#include <QObject>
#include <map>
#include <memory>
#include <set>
#include <string>
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
    explicit PitchCurves(QObject* parent = nullptr);
    // The clip's pitch, or null while it is being worked out (ready() follows).
    std::shared_ptr<const Curve> curve(const Project& p, const Sequence& s, const Clip& c);
    // Worked out now (for tests and scripts): the same, waiting for it.
    std::shared_ptr<const Curve> curveNow(const Project& p, const Sequence& s, const Clip& c);

signals:
    void ready();

private:
    std::string keyOf(const Project& p, const Sequence& s, const Clip& c) const;
    std::shared_ptr<const Curve> finish(const std::string& key, const std::shared_ptr<const PitchTrack>& track, const Clip& c);
    std::map<std::string, std::shared_ptr<const PitchTrack>> tracks_;
    std::map<std::string, std::shared_ptr<const Curve>> curves_;  // by track key and the edits
    std::set<std::string> running_;
};

}  // namespace montage
