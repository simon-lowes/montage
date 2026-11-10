#include "WaylandColor.h"

#ifdef MONTAGE_WAYLAND_COLOR

#include <unistd.h>
#include <wayland-client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "color-management-v1-client-protocol.h"

namespace montage {

namespace {

// Version 1 of every interface: its events are the ones handled here, whichever revision of the protocol's XML the
// code was generated from.
constexpr uint32_t kVersion = 1;

// The mastering display Montage's HDR10 files state (render/Exporter): P3-D65, in millionths.
constexpr int32_t kP3D65[8] = {680000, 320000, 265000, 690000, 150000, 60000, 312700, 329000};

}  // namespace

struct WaylandColor::State {
    wl_display* display = nullptr;
    wl_event_queue* queue = nullptr;
    wl_display* wrapper = nullptr;  // the display, its new objects on `queue`
    wl_registry* registry = nullptr;
    wp_color_manager_v1* manager = nullptr;
    uint32_t features = 0, tfs = 0, primaries = 0, intents = 0;  // bit sets of the protocol's enum values
    bool managerDone = false;

    wl_surface* surface = nullptr;
    wp_color_management_surface_v1* cmSurface = nullptr;
    wp_color_management_surface_feedback_v1* feedback = nullptr;
    bool preferredChanged = false;
    Preferred preferred;
    bool hasDescribed = false;
    Description described;

    // An image description being made or read: ready or failed, and what get_information told.
    struct Pending {
        bool ready = false, failed = false;
        std::string why;
        bool infoDone = false;
        Preferred info;
    };

    bool roundtrip() { return display && queue && wl_display_roundtrip_queue(display, queue) >= 0; }
    bool has(uint32_t set, uint32_t value) const { return value < 32 && (set & (1u << value)); }

    // Waits for the description to be ready or to fail (a roundtrip at a time: the compositor answers at once).
    bool waitFor(Pending& p) {
        for (int i = 0; i < 4 && !p.ready && !p.failed; ++i)
            if (!roundtrip()) return false;
        return p.ready;
    }

    void readPreferred();
};

namespace {

using Preferred = WaylandColor::Preferred;

void addBit(uint32_t& set, uint32_t value) {
    if (value < 32) set |= 1u << value;
}

void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto* s = static_cast<WaylandColor::State*>(data);
    if (!s->manager && std::strcmp(interface, wp_color_manager_v1_interface.name) == 0 && version >= kVersion)
        s->manager = static_cast<wp_color_manager_v1*>(wl_registry_bind(registry, name, &wp_color_manager_v1_interface, kVersion));
}

void registryGlobalRemove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener kRegistryListener = {registryGlobal, registryGlobalRemove};

const wp_color_manager_v1_listener kManagerListener = [] {
    wp_color_manager_v1_listener l{};
    l.supported_intent = [](void* data, wp_color_manager_v1*, uint32_t v) { addBit(static_cast<WaylandColor::State*>(data)->intents, v); };
    l.supported_feature = [](void* data, wp_color_manager_v1*, uint32_t v) { addBit(static_cast<WaylandColor::State*>(data)->features, v); };
    l.supported_tf_named = [](void* data, wp_color_manager_v1*, uint32_t v) { addBit(static_cast<WaylandColor::State*>(data)->tfs, v); };
    l.supported_primaries_named = [](void* data, wp_color_manager_v1*, uint32_t v) {
        addBit(static_cast<WaylandColor::State*>(data)->primaries, v);
    };
    l.done = [](void* data, wp_color_manager_v1*) { static_cast<WaylandColor::State*>(data)->managerDone = true; };
    return l;
}();

const wp_image_description_v1_listener kDescriptionListener = [] {
    wp_image_description_v1_listener l{};
    l.failed = [](void* data, wp_image_description_v1*, uint32_t cause, const char* msg) {
        auto* p = static_cast<WaylandColor::State::Pending*>(data);
        p->failed = true;
        p->why = msg && *msg ? msg : "cause " + std::to_string(cause);
    };
    l.ready = [](void* data, wp_image_description_v1*, uint32_t) { static_cast<WaylandColor::State::Pending*>(data)->ready = true; };
    return l;
}();

const wp_image_description_info_v1_listener kInfoListener = [] {
    using Pending = WaylandColor::State::Pending;
    wp_image_description_info_v1_listener l{};
    l.done = [](void* data, wp_image_description_info_v1*) { static_cast<Pending*>(data)->infoDone = true; };
    l.icc_file = [](void*, wp_image_description_info_v1*, int32_t fd, uint32_t) { close(fd); };
    l.primaries = [](void*, wp_image_description_info_v1*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) {};
    l.primaries_named = [](void*, wp_image_description_info_v1*, uint32_t) {};
    l.tf_power = [](void*, wp_image_description_info_v1*, uint32_t) {};
    l.tf_named = [](void* data, wp_image_description_info_v1*, uint32_t tf) { static_cast<Pending*>(data)->info.tf = tf; };
    l.luminances = [](void* data, wp_image_description_info_v1*, uint32_t minLum, uint32_t maxLum, uint32_t referenceLum) {
        Preferred& p = static_cast<Pending*>(data)->info;
        p.minNits = minLum / 10000.0;
        p.maxNits = maxLum;
        p.referenceNits = referenceLum;
    };
    l.target_primaries = [](void*, wp_image_description_info_v1*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) {};
    l.target_luminance = [](void* data, wp_image_description_info_v1*, uint32_t, uint32_t maxLum) {
        static_cast<Pending*>(data)->info.targetMaxNits = maxLum;
    };
    l.target_max_cll = [](void* data, wp_image_description_info_v1*, uint32_t v) { static_cast<Pending*>(data)->info.targetMaxCll = v; };
    l.target_max_fall = [](void*, wp_image_description_info_v1*, uint32_t) {};
    return l;
}();

const wp_color_management_surface_feedback_v1_listener kFeedbackListener = [] {
    wp_color_management_surface_feedback_v1_listener l{};
    l.preferred_changed = [](void* data, wp_color_management_surface_feedback_v1*, uint32_t) {
        static_cast<WaylandColor::State*>(data)->preferredChanged = true;
    };
    return l;
}();

}  // namespace

void WaylandColor::State::readPreferred() {
    preferred = Preferred();
    if (!feedback) return;
    Pending p;
    wp_image_description_v1* d = wp_color_management_surface_feedback_v1_get_preferred(feedback);
    wp_image_description_v1_add_listener(d, &kDescriptionListener, &p);
    if (waitFor(p)) {
        wp_image_description_info_v1* info = wp_image_description_v1_get_information(d);
        wp_image_description_info_v1_add_listener(info, &kInfoListener, &p);
        for (int i = 0; i < 4 && !p.infoDone; ++i)
            if (!roundtrip()) break;
        wp_image_description_info_v1_destroy(info);  // (the compositor's side went with `done`)
        if (p.infoDone && p.info.referenceNits > 0) {
            preferred = p.info;
            preferred.known = true;
        }
    }
    wp_image_description_v1_destroy(d);
}

WaylandColor::WaylandColor(wl_display* display) : s_(std::make_unique<State>()) {
    State& s = *s_;
    if (!display) return;
    s.display = display;
    s.queue = wl_display_create_queue(display);
    s.wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(display));
    if (!s.queue || !s.wrapper) return;
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(s.wrapper), s.queue);
    s.registry = wl_display_get_registry(s.wrapper);
    wl_registry_add_listener(s.registry, &kRegistryListener, &s);
    if (!s.roundtrip() || !s.manager) return;
    wp_color_manager_v1_add_listener(s.manager, &kManagerListener, &s);
    for (int i = 0; i < 4 && !s.managerDone; ++i)
        if (!s.roundtrip()) break;
}

WaylandColor::~WaylandColor() {
    State& s = *s_;
    detach();
    if (s.manager) wp_color_manager_v1_destroy(s.manager);
    if (s.registry) wl_registry_destroy(s.registry);
    if (s.wrapper) wl_proxy_wrapper_destroy(s.wrapper);
    if (s.display) wl_display_flush(s.display);
    if (s.queue) wl_event_queue_destroy(s.queue);
}

bool WaylandColor::available() const { return s_->manager && s_->managerDone; }

std::string WaylandColor::unsupported() const {
    const State& s = *s_;
    if (!available()) return "no colour manager";
    if (!s.has(s.features, WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC)) return "no parametric image descriptions";
    if (!s.has(s.tfs, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ)) return "no PQ transfer function";
    if (!s.has(s.primaries, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020) && !s.has(s.features, WP_COLOR_MANAGER_V1_FEATURE_SET_PRIMARIES))
        return "no BT.2020 primaries";
    if (!s.has(s.intents, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL)) return "no perceptual rendering intent";
    return {};
}

bool WaylandColor::supportsHlg() const { return s_->has(s_->tfs, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG); }

std::string WaylandColor::features() const {
    const State& s = *s_;
    if (!available()) return "none";
    std::string out = "features";
    for (uint32_t i = 0; i < 32; ++i)
        if (s.has(s.features, i)) out += " " + std::to_string(i);
    out += ", transfer functions";
    for (uint32_t i = 0; i < 32; ++i)
        if (s.has(s.tfs, i)) out += " " + std::to_string(i);
    out += ", primaries";
    for (uint32_t i = 0; i < 32; ++i)
        if (s.has(s.primaries, i)) out += " " + std::to_string(i);
    return out;
}

bool WaylandColor::attach(wl_surface* surface) {
    State& s = *s_;
    if (surface == s.surface && s.cmSurface) return true;
    detach();
    if (!surface || !available()) return false;
    s.surface = surface;
    s.cmSurface = wp_color_manager_v1_get_surface(s.manager, surface);
    s.feedback = wp_color_manager_v1_get_surface_feedback(s.manager, surface);
    wp_color_management_surface_feedback_v1_add_listener(s.feedback, &kFeedbackListener, &s);
    s.preferredChanged = false;
    s.readPreferred();
    return wl_display_get_error(s.display) == 0;
}

void WaylandColor::detach() {
    State& s = *s_;
    // (destroying is allowed when the wl_surface has gone: the objects are inert then)
    if (s.feedback) wp_color_management_surface_feedback_v1_destroy(s.feedback);
    if (s.cmSurface) wp_color_management_surface_v1_destroy(s.cmSurface);
    s.feedback = nullptr;
    s.cmSurface = nullptr;
    s.surface = nullptr;
    s.hasDescribed = false;
    s.preferred = Preferred();
    if (s.display) wl_display_flush(s.display);
}

wl_surface* WaylandColor::surface() const { return s_->surface; }

bool WaylandColor::describe(const Description& d, std::string* error) {
    State& s = *s_;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (!s.cmSurface) return fail("no surface");
    if (const std::string why = unsupported(); !why.empty()) return fail(why);
    wp_image_description_creator_params_v1* c = wp_color_manager_v1_create_parametric_creator(s.manager);
    const bool hlg = d.transfer == Transfer::Hlg && supportsHlg();
    wp_image_description_creator_params_v1_set_tf_named(
        c, hlg ? WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG : WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
    if (s.has(s.primaries, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020))
        wp_image_description_creator_params_v1_set_primaries_named(c, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
    else  // BT.2020's chromaticities (red, green, blue, D65), in millionths
        wp_image_description_creator_params_v1_set_primaries(c, 708000, 292000, 170000, 797000, 131000, 46000, 312700, 329000);
    // (the luminances are PQ's and HLG's defaults: 0.005 nits, 10 000 or 1000, and reference white at 203)
    if (s.has(s.features, WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES)) {
        wp_image_description_creator_params_v1_set_mastering_display_primaries(c, kP3D65[0], kP3D65[1], kP3D65[2], kP3D65[3], kP3D65[4],
                                                                               kP3D65[5], kP3D65[6], kP3D65[7]);
        wp_image_description_creator_params_v1_set_mastering_luminance(c, uint32_t(std::lround(d.light.masteringMinNits * 10000)),
                                                                       uint32_t(std::lround(d.light.masteringMaxNits)));
    }
    if (d.light.maxCll > 0) wp_image_description_creator_params_v1_set_max_cll(c, uint32_t(std::lround(d.light.maxCll)));
    if (d.light.maxFall > 0) wp_image_description_creator_params_v1_set_max_fall(c, uint32_t(std::lround(d.light.maxFall)));
    State::Pending p;
    wp_image_description_v1* desc = wp_image_description_creator_params_v1_create(c);  // (the creator goes with it)
    wp_image_description_v1_add_listener(desc, &kDescriptionListener, &p);
    const bool ready = s.waitFor(p);
    if (ready) wp_color_management_surface_v1_set_image_description(s.cmSurface, desc, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
    wp_image_description_v1_destroy(desc);  // (set_image_description copies it)
    wl_display_flush(s.display);
    if (!ready) return fail(p.failed ? p.why : "no answer from the compositor");
    s.described = d;
    s.hasDescribed = true;
    return true;
}

const WaylandColor::Description* WaylandColor::described() const { return s_->hasDescribed ? &s_->described : nullptr; }

const WaylandColor::Preferred& WaylandColor::preferred() const { return s_->preferred; }

bool WaylandColor::dispatch() {
    State& s = *s_;
    if (!s.display || !s.queue) return false;
    wl_display_dispatch_queue_pending(s.display, s.queue);
    if (!s.preferredChanged || !s.feedback) return false;
    s.preferredChanged = false;
    const Preferred before = s.preferred;
    s.readPreferred();
    const Preferred& now = s.preferred;
    return now.known != before.known || now.tf != before.tf || now.referenceNits != before.referenceNits || now.maxNits != before.maxNits ||
           now.targetMaxNits != before.targetMaxNits;
}

}  // namespace montage

#endif
