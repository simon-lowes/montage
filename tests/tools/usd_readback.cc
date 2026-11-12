// Reads a USD scene with tinyusdz (lighttransport/tinyusdz, Apache-2.0; its library is called lightusd), for the
// test that checks Montage's USD exports against a reader other than Montage's own. Built only by
// scripts/build-tinyusdz.sh, never into Montage.
//
//   usd_readback <scene.usda|.usdc|.usdz> <out-dir> <time code>...
//
// For each mesh at each time code: "xform <name> <time> <16 numbers>", its transform to the world (row vectors, as
// USD writes a matrix4d). For each mesh with a picture on its UsdPreviewSurface's diffuse colour, once:
// "picture <name> <width> <height> <channels> <file>", the picture's texels as tinyusdz decoded them, written raw
// (8 bits a channel: an 8-bit picture it gives as floats is put back to its bytes).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "lightusd.hh"
#include "tydra/render-data-converter.hh"
#include "tydra/render-data.hh"

namespace {

bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

std::string directoryOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// Each node's mesh (by its index) and its global matrix.
template <typename Nodes>
void meshNodes(const Nodes& nodes, std::vector<std::pair<int, const lightusd::value::matrix4d*>>& out) {
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& n = nodes[i];
        if (n.nodeType == lightusd::tydra::NodeType::Mesh && n.id >= 0) out.push_back({n.id, &n.global_matrix});
        meshNodes(n.children, out);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: usd_readback <scene> <out-dir> <time code>...\n";
        return 2;
    }
    const std::string file = argv[1], outDir = argv[2];
    const bool usdz = endsWith(file, ".usdz") || endsWith(file, ".USDZ");
    std::set<std::string> written;
    for (int a = 3; a < argc; ++a) {
        const double tc = std::strtod(argv[a], nullptr);
        lightusd::Stage stage;
        std::string warn, err;
        if (!lightusd::LoadUSDFromFile(file, &stage, &warn, &err)) {
            std::cerr << "cannot load " << file << ": " << err << "\n";
            return 1;
        }
        lightusd::tydra::RenderSceneConverter converter;
        lightusd::tydra::RenderSceneConverterEnv env(stage);
        env.scene_config.load_texture_assets = true;
        lightusd::USDZAsset asset;  // (must live until the conversion is done)
        if (usdz) {
            if (!lightusd::ReadUSDZAssetInfoFromFile(file, &asset, &warn, &err)) {
                std::cerr << "cannot read the package " << file << ": " << err << "\n";
                return 1;
            }
            lightusd::AssetResolutionResolver resolver;
            if (!lightusd::SetupUSDZAssetResolution(resolver, &asset)) {
                std::cerr << "cannot resolve assets in " << file << "\n";
                return 1;
            }
            env.asset_resolver = resolver;
        } else {
            env.set_search_paths({directoryOf(file)});
        }
        env.timecode = tc;
        lightusd::tydra::RenderScene scene;
        if (!converter.ConvertToRenderScene(env, &scene)) {
            std::cerr << "cannot convert " << file << ": " << converter.GetError() << "\n";
            return 1;
        }
        std::vector<std::pair<int, const lightusd::value::matrix4d*>> meshes;
        meshNodes(scene.nodes, meshes);
        for (const auto& [id, m] : meshes) {
            if (size_t(id) >= scene.meshes.size()) continue;
            const lightusd::tydra::RenderMesh& mesh = scene.meshes[size_t(id)];
            std::printf("xform %s %.17g", mesh.prim_name.c_str(), tc);
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) std::printf(" %.17g", m->m[r][c]);
            std::printf("\n");
            // Its picture, once.
            if (written.count(mesh.prim_name) || mesh.material_id < 0 || size_t(mesh.material_id) >= scene.materials.size()) continue;
            const auto& mat = scene.materials[size_t(mesh.material_id)];
            if (!mat.surfaceShader.has_value()) continue;
            const int tex = mat.surfaceShader->diffuseColor.texture_id;
            if (tex < 0 || size_t(tex) >= scene.textures.size()) continue;
            const int64_t im = scene.textures[size_t(tex)].texture_image_id;
            if (im < 0 || size_t(im) >= scene.images.size()) continue;
            const lightusd::tydra::TextureImage& img = scene.images[size_t(im)];
            if (img.buffer_id < 0 || size_t(img.buffer_id) >= scene.buffers.size() || img.width <= 0 || img.height <= 0 || img.channels <= 0) continue;
            const auto& data = scene.buffers[size_t(img.buffer_id)].data;
            const size_t n = size_t(img.width) * size_t(img.height) * size_t(img.channels);
            std::vector<unsigned char> bytes(n);
            if (img.texelComponentType == lightusd::tydra::ComponentType::Float || data.size() == n * sizeof(float)) {
                if (data.size() < n * sizeof(float)) continue;
                for (size_t i = 0; i < n; ++i) {
                    float v;
                    std::memcpy(&v, data.data() + i * sizeof(float), sizeof(float));
                    bytes[i] = (unsigned char)std::lround(std::fmin(1.0f, std::fmax(0.0f, v)) * 255);
                }
            } else {
                if (data.size() < n) continue;
                std::memcpy(bytes.data(), data.data(), n);
            }
            const std::string out = outDir + "/" + mesh.prim_name + ".raw";
            std::ofstream f(out, std::ios::binary);
            f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(n));
            if (!f) {
                std::cerr << "cannot write " << out << "\n";
                return 1;
            }
            written.insert(mesh.prim_name);
            std::printf("picture %s %d %d %d %s\n", mesh.prim_name.c_str(), img.width, img.height, img.channels, out.c_str());
        }
    }
    return 0;
}
