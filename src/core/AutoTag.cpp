#include "AutoTag.h"

#include <algorithm>
#include <cmath>

namespace montage {

const std::vector<TagCategory>& tagCategories() {
    static const std::vector<TagCategory> categories{
        {"Shot",
         {{"Close-up", {"a close-up photo of a face", "an extreme close-up shot", "a close-up shot of hands or an object"}},
          {"Medium shot", {"a medium shot of a person from the waist up", "a medium shot of two people talking"}},
          {"Wide shot", {"a wide shot of a landscape", "a wide establishing shot of a building", "a long shot of people far away"}}}},
        {"Setting",
         {{"Interior", {"a photo taken indoors", "the interior of a room"}},
          {"Exterior", {"a photo taken outdoors", "an outdoor scene outside"}}}},
        {"Time", {{"Day", {"a photo taken in bright daylight", "a scene during the day"}}, {"Night", {"a photo taken at night", "a dark scene at night"}}}},
        {"People",
         {{"People", {"a photo of a person", "a photo of a group of people"}},
          {"", {"a photo with nobody in it", "an empty scene with no people"}}}},
    };
    return categories;
}

AutoTags autoTags(const VisualIndex& v, const LabelEmbeddings& labels, double from, double to, double confidence, double share) {
    AutoTags out;
    const auto& cats = tagCategories();
    std::vector<size_t> samples;
    for (size_t i = 0; i < v.samples.size(); ++i)
        if (v.samples[i].time >= from - 1e-9 && (to < 0 || v.samples[i].time <= to + 1e-9)) samples.push_back(i);
    if (samples.empty()) return out;
    const double half = (v.step > 0 ? v.step : 1.0) / 2;
    for (size_t c = 0; c < cats.size() && c < labels.size(); ++c) {
        const size_t n = std::min(cats[c].labels.size(), labels[c].size());
        if (n == 0) continue;
        // The label each sample takes, or none.
        std::vector<int> taken(samples.size(), -1);
        std::vector<size_t> count(n, 0);
        for (size_t k = 0; k < samples.size(); ++k) {
            std::vector<double> logits(n);
            double top = -1e9;
            for (size_t l = 0; l < n; ++l) top = std::max(top, logits[l] = 100.0 * v.similarity(samples[k], labels[c][l]));
            double sum = 0;
            for (double& x : logits) sum += x = std::exp(x - top);
            const size_t best = size_t(std::max_element(logits.begin(), logits.end()) - logits.begin());
            if (logits[best] / sum >= confidence) {
                taken[k] = int(best);
                ++count[best];
            }
        }
        for (size_t l = 0; l < n; ++l)
            if (!cats[c].labels[l].keyword.empty() && double(count[l]) >= share * double(samples.size()))
                out.keywords.push_back(cats[c].labels[l].keyword);
        // Runs of two samples or more.
        for (size_t a = 0; a < samples.size();) {
            size_t b = a;
            while (b + 1 < samples.size() && taken[b + 1] == taken[a]) ++b;
            if (taken[a] >= 0 && b > a && !cats[c].labels[size_t(taken[a])].keyword.empty())
                out.runs.push_back({cats[c].labels[size_t(taken[a])].keyword, std::max(0.0, v.samples[samples[a]].time - half),
                                    v.samples[samples[b]].time + half});
            a = b + 1;
        }
    }
    std::sort(out.runs.begin(), out.runs.end(), [](const TagRun& a, const TagRun& b) { return a.start < b.start; });
    return out;
}

AutoTags autoTagMedia(const Project& p, const MediaItem& m, const LabelEmbeddings& labels) {
    if (m.subclipOf) {
        const MediaItem* parent = p.findMedia(m.subclipOf);
        if (!parent || !parent->visual) return {};
        return autoTags(*parent->visual, labels, m.subclipIn, m.subclipOut);
    }
    return m.visual ? autoTags(*m.visual, labels) : AutoTags{};
}

}  // namespace montage
