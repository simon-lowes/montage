// Montage — tagging footage by what it shows, without a query. Each sample
// of a video's visual index is compared with a few descriptions of every
// label in a category (shot size, interior or exterior, day or night,
// people), as CLIP's zero-shot classification does. A label that holds for
// enough of the footage becomes a keyword, and runs of it are reported.
#pragma once

#include <string>
#include <vector>

#include "Model.h"
#include "VisualIndex.h"

namespace montage {

struct TagLabel {
    std::string keyword;               // "" for a label that is only there to compete ("no people")
    std::vector<std::string> prompts;  // descriptions whose embeddings are averaged
};
struct TagCategory {
    std::string name;
    std::vector<TagLabel> labels;
};
const std::vector<TagCategory>& tagCategories();

// Unit-length text embeddings: [category][label], in tagCategories() order.
using LabelEmbeddings = std::vector<std::vector<std::vector<float>>>;

struct TagRun {
    std::string keyword;
    double start = 0, end = 0;  // media seconds
};
struct AutoTags {
    std::vector<std::string> keywords;  // in category order
    std::vector<TagRun> runs;           // stretches of at least two samples, in time order
};

// Tags the samples between `from` and `to` media seconds (to < 0: all). A
// sample takes a label when CLIP's softmax over its category (logit scale
// 100) gives it at least `confidence`; a keyword needs at least `share` of
// the samples.
AutoTags autoTags(const VisualIndex& v, const LabelEmbeddings& labels, double from = 0, double to = -1,
                  double confidence = 0.6, double share = 0.4);
// A media item's tags from its index; a subclip's come from its media's index
// within its range. Nothing if there is no index.
AutoTags autoTagMedia(const Project& p, const MediaItem& m, const LabelEmbeddings& labels);

}  // namespace montage
