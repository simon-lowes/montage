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
// samples close to their media's best score are joined into one moment.
std::vector<ShotMatch> findShots(const Project& p, const std::vector<float>& query, size_t max = 20);

}  // namespace montage
