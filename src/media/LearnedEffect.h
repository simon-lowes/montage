// Montage — an effect learned from examples: a few frames of a shot worked on by hand (painted, graded, cleaned up, a
// matte drawn) and the same frames as they were, a small network trained on them on this computer, and the network run
// over the rest of the shot or similar shots.
//
// The network is a U-Net of its own (four levels of two 3x3 convolutions with leaky ReLUs, 16 to 128 channels, average
// pooling down and nearest-neighbour up, skips joined; about 0.5 M parameters) on straight RGBA, and its last layer adds
// to the picture: a 1x1 convolution that starts at zero, so an untrained network leaves the picture alone. It is trained
// here (the convolutions, back-propagation and Adam written for the processor, threaded) on crops of the examples with
// flips, by mean squared error. Every sum is taken in the same order whatever the number of threads, so the same seed
// gives the same weights on any. The trained network is written as an ONNX graph built here, and run through ONNX
// Runtime in tiles (on the processor or a GPU), or by the trainer's own code without it.
#pragma once

#include <atomic>
#include <memory>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Image.h"

namespace montage::learn {

struct NetShape {
    int levels = 4;    // 1..6; frames are padded to a multiple of 2^(levels - 1)
    int base = 16;     // channels at the top level (a multiple of 4), doubled at each level down
    int channels = 4;  // RGBA in and out
    bool operator==(const NetShape&) const = default;
};
size_t parameterCount(const NetShape& s);

struct Net {
    NetShape shape;
    std::vector<float> weights;  // each convolution's weights [out][in][ky][kx] then its biases, top to bottom
    // He-initialised from `seed`, the last layer zero.
    void init(const NetShape& s, uint64_t seed);
};

// The network on planar float channels (channels x h x w; h and w multiples of 2^(levels - 1)). `threads` 0: all.
void forward(const Net& net, const float* in, int w, int h, float* out, int threads = 0);
// The mean squared error of the network's output against `target` (planar as `in`), and its gradient with respect to
// every weight (added to `grad`, sized as the weights).
double lossAndGradient(const Net& net, const float* in, const float* target, int w, int h, std::vector<float>& grad, int threads = 0);

// The network on a picture of any size (straight RGBA), on the processor: padded by reflection, in tiles.
Image applyNet(const Net& net, const Image& in, int threads = 0, const std::atomic<bool>* cancel = nullptr);

// The network as an ONNX model (IR 8, opset 13): "input" [1, channels, H, W] to "output", H and W free (multiples of
// 2^(levels - 1)).
std::vector<unsigned char> onnxModel(const Net& net);
// Through ONNX Runtime (the device media/Inference.h chooses), in tiles; false (with `error`) without it or on failure.
bool applyNetOrt(const Net& net, const Image& in, Image& out, std::string* error = nullptr, const std::atomic<bool>* cancel = nullptr);
bool onnxRuntimeAvailable();
// An ONNX model of onnxModel()'s form run once on planar channels (as forward()).
bool runOnnx(const std::vector<unsigned char>& model, const float* in, int channels, int w, int h, std::vector<float>& out,
             std::string* error = nullptr);

// A frame of the shot as it was, and as worked on by hand (the same size).
struct Pair {
    Image before, after;
    int64_t frame = 0;  // where it came from (kept with the network)
};

struct TrainOptions {
    int crop = 128;  // square crops (a multiple of 2^(levels - 1); smaller pairs give their own size)
    int batch = 1;
    double rate = 2e-3;
    double decay = 500;  // the rate falls as rate / (1 + step / decay), so long runs settle (0: it stays)
    uint64_t seed = 1;
    int threads = 0;  // 0: all
};

// Trains a network on the pairs, a step at a time; stopped and continued (through state()) it goes on as if it had
// not been.
class Trainer {
public:
    Trainer(const NetShape& shape, std::vector<Pair> pairs, const TrainOptions& options);
    // `steps` more steps (each a batch of crops); false when cancelled (the steps done kept). `progress` is told after
    // each step.
    bool run(int steps, const std::atomic<bool>* cancel = nullptr, const std::function<void(int step, double loss)>& progress = {});
    int steps() const { return step_; }
    double loss() const { return loss_; }  // the last step's mean squared error
    const Net& net() const { return net_; }
    const std::vector<Pair>& pairs() const { return pairs_; }
    const TrainOptions& options() const { return options_; }

    // Everything to go on from here (the weights, Adam's moments, the step and the random state), and back.
    std::vector<unsigned char> state() const;
    bool restore(const std::vector<unsigned char>& state);
    static int stepsIn(const std::vector<unsigned char>& state);  // the steps a state() was trained

private:
    NetShape shape_;
    std::vector<Pair> pairs_;
    TrainOptions options_;
    Net net_;
    std::vector<float> m_, v_;
    int step_ = 0;
    double loss_ = 0;
    uint64_t random_ = 0;
};

// A trained network saved with the frames it learned from (and its training state, to go on later).
struct Saved {
    Net net;
    std::vector<int64_t> frames;
    TrainOptions options;
    std::vector<unsigned char> state;  // Trainer::state(); empty: not to be continued
    std::string source;                // what it learned from (a clip's name), shown
};
bool save(const std::string& path, const Saved& s, std::string* error = nullptr);
bool load(const std::string& path, Saved& s, std::string* error = nullptr);
// A saved network by path, read once and kept (changed files read again).
std::shared_ptr<const Saved> cachedNet(const std::string& path, std::string* error = nullptr);

// The network saved at `file` on a picture: through ONNX Runtime when it is there (on the device media/Inference.h
// chooses), else the processor; the last results (a few hundred megabytes) kept by what went in. With `run` false only
// a result already made is given (false, with no error, when there is none).
bool applySaved(const std::string& file, const Image& in, Image& out, std::string* error = nullptr, const std::atomic<bool>* cancel = nullptr,
                bool run = true);

// The peak signal-to-noise ratio of `a` against `b` (RGB, 0..1), dB.
double psnr(const Image& a, const Image& b);

}  // namespace montage::learn
