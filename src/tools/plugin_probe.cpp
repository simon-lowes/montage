// montage-plugin-probe <format> <path>
//
// Loads one plugin file and prints the plugins inside it as JSON on stdout.
// Montage runs this in a separate process for every new or changed plugin,
// so a plugin that crashes or hangs while loading only takes this helper down.
#include <cstdio>
#include <string>

#include "audio/Plugins.h"
#include "render/Ofx.h"

int main(int argc, char** argv) {
    using namespace montage::plugins;
    if (argc != 3) {
        std::fprintf(stderr, "usage: montage-plugin-probe <CLAP|VST3|LV2|AU|OFX> <path>\n");
        return 2;
    }
    // OpenFX video plugins: the image effects in one binary, described as filters.
    if (std::string(argv[1]) == "OFX") {
        std::string error;
        const auto effects = montage::ofx::describeBinary(argv[2], &error);
        if (effects.empty()) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        const std::string json = montage::ofx::descriptionsToJson(effects);
        std::fwrite(json.data(), 1, json.size(), stdout);
        std::fputc('\n', stdout);
        return 0;
    }
    auto format = formatFromName(argv[1]);
    if (!format) {
        std::fprintf(stderr, "unknown plugin format: %s\n", argv[1]);
        return 2;
    }
    std::string error;
    auto plugins = probeInProcess(*format, argv[2], &error);
    if (!error.empty()) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::string json = descriptorsToJson(plugins);
    std::fwrite(json.data(), 1, json.size(), stdout);
    std::fputc('\n', stdout);
    return 0;
}
