// Montage — OpenFX renders in a plug-in host process (audio/PluginHost.h): the effect, the plug-in's description and
// the pictures go to montage-pluginhost, which renders as Montage would (render/Ofx.h) and sends the picture back;
// frames at other times and connected inputs are asked back from Montage while it renders. Internal to Ofx.cpp.
#pragma once

#include <QImage>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "render/Ofx.h"

namespace montage::ofx::remote {

// Whether renders go to a host (plug-ins are isolated, and this is not a host itself).
bool isolated();

enum class Kind { Effect = 0, Transition = 1, Generator = 2 };
// Renders `e` (its plug-in `d`) in its host: an effect on `a`, a transition from `a` to `b` (both w x h), or a
// generator's w x h picture. False, with why, when the plug-in failed (it is then bypassed: the picture stays as it
// came) or is bypassed after failing too often.
bool render(Kind kind, const Effect& e, const PluginDesc& d, double t, double progress, double scale, int w, int h, const Image* a,
            const Image* b, Image& out, const FrameFetch* fetch, std::string* error, const ColorSpace* working);

// Lets go of the channels kept for the next render, so hosts with nothing else in them end (and new ones start with
// the settings as they are then).
void releaseIdle();

// An effect's on-screen controls in its host: a channel of their own beside its renders', the overlay drawn there into
// the channel's shared block. Calls come from the GUI thread and wait at most kDeadlineMs; a reply later than that
// leaves the last picture up (unresponsive()) and is collected at the next call, and one that has not come after
// kHangMs ends the host as hung. Once a draw has been late, draws are not waited for until one comes in time again
// (each paint shows the picture the one before asked for). What a late pen or key reply set is handed on with the
// next event, or by lateEdits().
class Controls {
public:
    static constexpr int kDeadlineMs = 200;
    static constexpr int kHangMs = 5000;
    // Null, with why, when the controls cannot start (the plug-in has none: null, no error).
    static std::unique_ptr<Controls> open(const Effect& e, const PluginDesc& d, std::string* error);
    ~Controls();
    Controls(const Controls&) = delete;
    Controls& operator=(const Controls&) = delete;
    bool draw(const Effect& e, double t, const View& v, Overlay& out);
    bool pen(int kind, const Effect& e, double t, const View& v, double vx, double vy, double pressure, Effect& edited, bool& changed);
    bool key(bool down, int keysym, const std::string& text, const Effect& e, double t, const View& v, Effect& edited, bool& changed);
    void focus(bool gained, const Effect& e, double t, const View& v);
    bool failed() const { return failed_; }
    // A message is still out (waiting()), and for longer than the deadline (unresponsive()).
    bool waiting() const;
    bool unresponsive() const;
    // What a late pen or key reply set and no event has handed on yet: only those parameters and strings.
    bool lateEdits(Effect& changes);
    const std::string& failure() const { return failure_; }
    struct Link;

private:
    Controls() = default;
    // Sends `cmd` with the view and effect (`args` fills in the rest of its OverlayArgs) and waits up to the deadline
    // (`timeoutUs`, when given); a Result as an int (0: the reply came in time).
    int send(uint32_t cmd, const Effect& e, double t, const View& v, const std::function<void(void*)>& args, const std::string& text = {},
             int64_t timeoutUs = 0);
    // A reply to an earlier message that missed its deadline: taken now if it has come (false: still waiting).
    bool settle();
    void fail(int result);
    bool takeImage(double pixelRatio);
    bool readEdited(Effect& edited, bool& changed);
    bool takeLate(Effect& into);  // (merges what late replies set into `into`)
    std::unique_ptr<Link> link_;
    PluginDesc desc_;
    std::string bypass_;  // what a failure past its restarts bypasses (render/OfxRemote.cpp bypassKey)
    Effect sent_;  // the effect as the last message gave it
    QImage last_;  // the last picture drawn
    int64_t sentUs_ = 0;  // when the message still out went
    uint32_t pendingCmd_ = 0;  // which (0: none)
    double pendingRatio_ = 1;
    bool slowDraws_ = false;  // (a draw was late: draws are not waited for)
    Effect late_;             // what late pen and key replies set
    bool lateEdits_ = false;
    bool failed_ = false;
    std::string failure_;
};

}  // namespace montage::ofx::remote
