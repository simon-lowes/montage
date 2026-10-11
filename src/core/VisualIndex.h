// Montage — what footage shows, for searching it by description: frames
// sampled through each video, each as an image embedding that a text query
// can be compared with (media/VisualSearch.h computes both). Kept on the
// media item and saved with the project.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace montage {

struct Project;

struct VisualIndex {
    struct Sample {
        double time = 0;            // media seconds
        float scale = 0;            // embedding = values * scale
        std::vector<int8_t> values;
        bool operator==(const Sample&) const = default;
    };
    std::string model;  // the model the embeddings come from
    double step = 0;    // seconds between samples
    double span = 0;    // seconds each sample hears, centred on its time (a sound window); 0: an instant (a frame)
    std::vector<Sample> samples;

    // Adds a sample from a unit-length embedding (quantised to 8 bits).
    void add(double time, const std::vector<float>& embedding);
    // Cosine similarity of sample i with a unit-length query.
    float similarity(size_t i, const std::vector<float>& query) const;
    bool operator==(const VisualIndex&) const = default;
};

std::string visualIndexToJson(const VisualIndex& v);
bool visualIndexFromJson(const std::string& json, VisualIndex& out);

// A stretch of footage that matches a query.
struct ShotMatch {
    uint64_t media = 0;
    double start = 0, end = 0;  // media seconds
    double best = 0;            // time of the best-matching sample
    float score = 0;            // cosine similarity there
};
// The best moments across the project's indexed media, best first: runs of
// samples close to their media's best score are joined into one moment (each
// sample's span wide where it has one, else half a step either side; `best` is
// then where the first window starts, so playing from it hears the sound).
// `tolerance`: how far below a peak a neighbouring sample may score and still be part of its moment.
std::vector<ShotMatch> findShots(const Project& p, const std::vector<float>& query, size_t max = 20, float tolerance = 0.015f);
// The same over the media's sound indexes (media/SoundSearch.h): where the sound a query describes is heard.
// CLAP's scores spread wider than CLIP's, so moments join samples within a wider margin of their peak.
std::vector<ShotMatch> findSounds(const Project& p, const std::vector<float>& query, size_t max = 20, float tolerance = 0.03f);
// The moments most like a frame (its image embedding), best first, leaving out the moment it comes from
// (`fromMedia` at `fromSeconds`). Frames of one shot score alike, so moments are wider than for words.
std::vector<ShotMatch> findSimilarShots(const Project& p, const std::vector<float>& image, uint64_t fromMedia, double fromSeconds,
                                        size_t max = 20);

}  // namespace montage
