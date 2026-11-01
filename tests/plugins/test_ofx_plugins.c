/* Montage's test OpenFX plugins, built as an .ofx.bundle:
 *  - Test Invert: inverts the (premultiplied) picture by Amount, then multiplies by Tint; Mode "Pass" leaves it alone.
 *  - Test Temporal Average: the average of the frames before, at and after the one rendered (temporal clip access).
 *  - Test Probe: shows what the host gave it. Mode "Centre" writes its Centre parameter (defined in normalised
 *    coordinates, read in canonical pixels) as red = x / 1000, green = y / 1000; mode "Changes" writes red = the
 *    times this instance was told Amount changed / 10.
 *  - Test Colour: takes OpenFX 1.5 colour management, asking for ACEScg. Mode "Report" writes the red it was given,
 *    green = 1 when the host agreed the core style, blue = 1 when its source is ACEScg, and says its output
 *    is raw; mode "Pass" gives its source back, in its source's colourspace.
 *  - Test Dissolve (a transition): from * (1 - Transition) + to * Transition.
 *  - Test Generator (a generator): Colour left of Split (a fraction of the frame's width), transparent right of it.
 *  - Test Matte (General context): its source times the alpha of its optional "Matte" mask input (the source as it
 *    is when nothing is connected).
 * Test Probe also has on-screen controls (drawn with the Draw Suite): a red square of 20 screen pixels round Centre
 * with a yellow cross through it; a press within the square and a drag move Centre. With MONTAGE_TEST_OFX_GL defined,
 * Test Invert also renders with OpenGL (the same formula in a shader) and has OpenGL on-screen controls: a green
 * square from canonical (100, 100) to (200, 200).
 * With MONTAGE_TEST_OFX_CRASH defined it instead crashes while describing itself, for the scanner's blocklist. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#endif

#include "ofxColour.h"
#include "ofxCore.h"
#include "ofxDrawSuite.h"
#include "ofxImageEffect.h"
#include "ofxInteract.h"
#include "ofxParam.h"
#include "ofxProperty.h"
#ifdef MONTAGE_TEST_OFX_GL
#include "ofxGPURender.h"
#ifdef _WIN32
#include <GL/gl.h>
#else
#include <GL/gl.h>
#include <dlfcn.h>
#endif
#endif

#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

static OfxHost* gHost;
static OfxPropertySuiteV1* gProp;
static OfxImageEffectSuiteV1* gEffect;
static OfxParameterSuiteV1* gParam;
static OfxDrawSuiteV1* gDraw;

static void setHost(OfxHost* h) {
    gHost = h;
}

static OfxStatus load(void) {
    gProp = (OfxPropertySuiteV1*)gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1);
    gEffect = (OfxImageEffectSuiteV1*)gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1);
    gParam = (OfxParameterSuiteV1*)gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1);
    gDraw = (OfxDrawSuiteV1*)gHost->fetchSuite(gHost->host, kOfxDrawSuite, 1);
    return gProp && gEffect && gParam ? kOfxStatOK : kOfxStatErrMissingHostFeature;
}

static OfxStatus describeAs(OfxImageEffectHandle effect, const char* label, int temporal, const char* context);
static OfxStatus describe(OfxImageEffectHandle effect, const char* label, int temporal) {
    return describeAs(effect, label, temporal, kOfxImageEffectContextFilter);
}

static OfxStatus describeAs(OfxImageEffectHandle effect, const char* label, int temporal, const char* context) {
#ifdef MONTAGE_TEST_OFX_CRASH
    (void)effect, (void)label, (void)temporal, (void)context;
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 3);  // like a crash, with no crash dialog on CI
#else
    raise(SIGSEGV);
#endif
    return kOfxStatFailed;
#else
    OfxPropertySetHandle props;
    gEffect->getPropertySet(effect, &props);
    gProp->propSetString(props, kOfxPropLabel, 0, label);
    gProp->propSetString(props, kOfxImageEffectPluginPropGrouping, 0, "Montage Test");
    gProp->propSetString(props, kOfxImageEffectPropSupportedContexts, 0, context);
    gProp->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat);
    gProp->propSetInt(props, kOfxImageEffectPropTemporalClipAccess, 0, temporal);
    gProp->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderFullySafe);
    return kOfxStatOK;
#endif
}

static OfxStatus describeInContext(OfxImageEffectHandle effect, int invert) {
    OfxPropertySetHandle clip;
    gEffect->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &clip);
    gProp->propSetString(clip, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gEffect->clipDefine(effect, kOfxImageEffectOutputClipName, &clip);
    gProp->propSetString(clip, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    if (!invert) return kOfxStatOK;
    OfxParamSetHandle params;
    gEffect->getParamSet(effect, &params);
    OfxPropertySetHandle p;
    gParam->paramDefine(params, kOfxParamTypeDouble, "amount", &p);
    gProp->propSetString(p, kOfxPropLabel, 0, "Amount");
    gProp->propSetDouble(p, kOfxParamPropDefault, 0, 1.0);
    gProp->propSetDouble(p, kOfxParamPropMin, 0, 0.0);
    gProp->propSetDouble(p, kOfxParamPropMax, 0, 1.0);
    gProp->propSetDouble(p, kOfxParamPropDisplayMin, 0, 0.0);
    gProp->propSetDouble(p, kOfxParamPropDisplayMax, 0, 1.0);
    gParam->paramDefine(params, kOfxParamTypeChoice, "mode", &p);
    gProp->propSetString(p, kOfxPropLabel, 0, "Mode");
    gProp->propSetString(p, kOfxParamPropChoiceOption, 0, "Invert");
    gProp->propSetString(p, kOfxParamPropChoiceOption, 1, "Pass");
    gProp->propSetInt(p, kOfxParamPropDefault, 0, 0);
    gParam->paramDefine(params, kOfxParamTypeRGB, "tint", &p);
    gProp->propSetString(p, kOfxPropLabel, 0, "Tint");
    gProp->propSetDouble(p, kOfxParamPropDefault, 0, 1.0);
    gProp->propSetDouble(p, kOfxParamPropDefault, 1, 1.0);
    gProp->propSetDouble(p, kOfxParamPropDefault, 2, 1.0);
    gParam->paramDefine(params, kOfxParamTypeString, "note", &p);
    gProp->propSetString(p, kOfxPropLabel, 0, "Note");
    gProp->propSetString(p, kOfxParamPropDefault, 0, "hello");
    return kOfxStatOK;
}

static OfxStatus describeProbe(OfxImageEffectHandle effect) {
    OfxPropertySetHandle clip;
    gEffect->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &clip);
    gProp->propSetString(clip, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gEffect->clipDefine(effect, kOfxImageEffectOutputClipName, &clip);
    gProp->propSetString(clip, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    OfxParamSetHandle params;
    gEffect->getParamSet(effect, &params);
    OfxPropertySetHandle p;
    gParam->paramDefine(params, kOfxParamTypeDouble2D, "centre", &p);
    gProp->propSetString(p, kOfxPropLabel, 0, "Centre");
    gProp->propSetString(p, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypeXYAbsolute);
    gProp->propSetString(p, kOfxParamPropDefaultCoordinateSystem, 0, kOfxParamCoordinatesNormalised);
    gProp->propSetDouble(p, kOfxParamPropDefault, 0, 0.5);
    gProp->propSetDouble(p, kOfxParamPropDefault, 1, 0.25);
    gProp->propSetDouble(p, kOfxParamPropDisplayMin, 0, 0.0);
    gProp->propSetDouble(p, kOfxParamPropDisplayMin, 1, 0.0);
    gProp->propSetDouble(p, kOfxParamPropDisplayMax, 0, 1.0);
    gProp->propSetDouble(p, kOfxParamPropDisplayMax, 1, 1.0);
    gParam->paramDefine(params, kOfxParamTypeDouble, "amount", &p);
    gProp->propSetString(p, kOfxPropLabel, 0, "Amount");
    gProp->propSetDouble(p, kOfxParamPropDefault, 0, 0.5);
    gParam->paramDefine(params, kOfxParamTypeChoice, "mode", &p);
    gProp->propSetString(p, kOfxPropLabel, 0, "Mode");
    gProp->propSetString(p, kOfxParamPropChoiceOption, 0, "Centre");
    gProp->propSetString(p, kOfxParamPropChoiceOption, 1, "Changes");
    return kOfxStatOK;
}

static int* changesOf(OfxImageEffectHandle effect) {
    OfxPropertySetHandle props;
    void* data = NULL;
    gEffect->getPropertySet(effect, &props);
    gProp->propGetPointer(props, kOfxPropInstanceData, 0, &data);
    return (int*)data;
}

typedef struct {
    float* data;
    int x1, y1, x2, y2, rowBytes;
} View;

static int viewOf(OfxPropertySetHandle img, View* v) {
    void* data = NULL;
    int bounds[4];
    if (gProp->propGetPointer(img, kOfxImagePropData, 0, &data) != kOfxStatOK || !data) return 0;
    gProp->propGetIntN(img, kOfxImagePropBounds, 4, bounds);
    gProp->propGetInt(img, kOfxImagePropRowBytes, 0, &v->rowBytes);
    v->data = (float*)data;
    v->x1 = bounds[0], v->y1 = bounds[1], v->x2 = bounds[2], v->y2 = bounds[3];
    return 1;
}

static float* pixel(const View* v, int x, int y) {
    return (float*)((char*)v->data + (ptrdiff_t)(y - v->y1) * v->rowBytes) + (x - v->x1) * 4;
}

static OfxStatus render(OfxImageEffectHandle effect, OfxPropertySetHandle in, int invert) {
    OfxTime time;
    int window[4];
    gProp->propGetDouble(in, kOfxPropTime, 0, &time);
    gProp->propGetIntN(in, kOfxImageEffectPropRenderWindow, 4, window);
    OfxImageClipHandle source, output;
    gEffect->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &source, NULL);
    gEffect->clipGetHandle(effect, kOfxImageEffectOutputClipName, &output, NULL);
    OfxPropertySetHandle outImg = NULL, srcImg = NULL, before = NULL, after = NULL;
    if (gEffect->clipGetImage(output, time, NULL, &outImg) != kOfxStatOK) return kOfxStatFailed;
    if (gEffect->clipGetImage(source, time, NULL, &srcImg) != kOfxStatOK) {
        gEffect->clipReleaseImage(outImg);
        return kOfxStatFailed;
    }
    View o, s, b, a;
    viewOf(outImg, &o);
    viewOf(srcImg, &s);
    double amount = 1, tint[3] = {1, 1, 1};
    int mode = 0;
    if (invert) {
        OfxParamSetHandle params;
        OfxParamHandle h;
        gEffect->getParamSet(effect, &params);
        gParam->paramGetHandle(params, "amount", &h, NULL);
        gParam->paramGetValueAtTime(h, time, &amount);
        gParam->paramGetHandle(params, "mode", &h, NULL);
        gParam->paramGetValueAtTime(h, time, &mode);
        gParam->paramGetHandle(params, "tint", &h, NULL);
        gParam->paramGetValueAtTime(h, time, &tint[0], &tint[1], &tint[2]);
    } else {
        gEffect->clipGetImage(source, time - 1, NULL, &before);
        gEffect->clipGetImage(source, time + 1, NULL, &after);
        viewOf(before, &b);
        viewOf(after, &a);
    }
    for (int y = window[1]; y < window[3]; ++y)
        for (int x = window[0]; x < window[2]; ++x) {
            const float* sp = pixel(&s, x, y);
            float* op = pixel(&o, x, y);
            if (!invert) {
                const float* bp = pixel(&b, x, y);
                const float* ap = pixel(&a, x, y);
                for (int c = 0; c < 4; ++c) op[c] = (bp[c] + sp[c] + ap[c]) / 3.0f;
                continue;
            }
            for (int c = 0; c < 3; ++c) {
                const float inv = sp[3] - sp[c];
                float v = mode == 1 ? sp[c] : (float)(sp[c] + (inv - sp[c]) * amount);
                op[c] = mode == 1 ? v : (float)(v * tint[c]);
            }
            op[3] = sp[3];
        }
    if (before) gEffect->clipReleaseImage(before);
    if (after) gEffect->clipReleaseImage(after);
    gEffect->clipReleaseImage(srcImg);
    gEffect->clipReleaseImage(outImg);
    return kOfxStatOK;
}

static OfxStatus renderProbe(OfxImageEffectHandle effect, OfxPropertySetHandle in) {
    OfxTime time;
    int window[4];
    gProp->propGetDouble(in, kOfxPropTime, 0, &time);
    gProp->propGetIntN(in, kOfxImageEffectPropRenderWindow, 4, window);
    OfxImageClipHandle output;
    gEffect->clipGetHandle(effect, kOfxImageEffectOutputClipName, &output, NULL);
    OfxPropertySetHandle outImg = NULL;
    if (gEffect->clipGetImage(output, time, NULL, &outImg) != kOfxStatOK) return kOfxStatFailed;
    View o;
    viewOf(outImg, &o);
    OfxParamSetHandle params;
    OfxParamHandle h;
    double cx = 0, cy = 0;
    int mode = 0;
    gEffect->getParamSet(effect, &params);
    gParam->paramGetHandle(params, "centre", &h, NULL);
    gParam->paramGetValueAtTime(h, time, &cx, &cy);
    gParam->paramGetHandle(params, "mode", &h, NULL);
    gParam->paramGetValueAtTime(h, time, &mode);
    const int* changes = changesOf(effect);
    const float r = mode == 1 ? (changes ? (float)*changes / 10.0f : -1.0f) : (float)(cx / 1000.0);
    const float g = mode == 1 ? 0.0f : (float)(cy / 1000.0);
    for (int y = window[1]; y < window[3]; ++y)
        for (int x = window[0]; x < window[2]; ++x) {
            float* op = pixel(&o, x, y);
            op[0] = r, op[1] = g, op[2] = 0, op[3] = 1;
        }
    gEffect->clipReleaseImage(outImg);
    return kOfxStatOK;
}

/* Test Probe's on-screen controls: the square round Centre (canonical pixels), and whether a press took it. */
static int gProbeGrabbed;

static OfxImageEffectHandle effectOfArgs(OfxPropertySetHandle in) {
    void* effect = NULL;
    gProp->propGetPointer(in, kOfxPropEffectInstance, 0, &effect);
    return (OfxImageEffectHandle)effect;
}

static OfxParamHandle probeCentre(OfxImageEffectHandle effect) {
    OfxParamSetHandle params;
    OfxParamHandle h = NULL;
    gEffect->getParamSet(effect, &params);
    gParam->paramGetHandle(params, "centre", &h, NULL);
    return h;
}

static OfxStatus probeOverlay(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out) {
    (void)handle, (void)out;
    if (!strcmp(action, kOfxActionDescribe) || !strcmp(action, kOfxActionCreateInstance) || !strcmp(action, kOfxActionDestroyInstance))
        return kOfxStatOK;
    const int draw = !strcmp(action, kOfxInteractActionDraw), down = !strcmp(action, kOfxInteractActionPenDown),
              motion = !strcmp(action, kOfxInteractActionPenMotion), up = !strcmp(action, kOfxInteractActionPenUp);
    if (!draw && !down && !motion && !up) return kOfxStatReplyDefault;
    OfxImageEffectHandle effect = effectOfArgs(in);
    if (!effect) return kOfxStatFailed;
    OfxTime time = 0;
    double ps[2] = {1, 1}, cx = 0, cy = 0;
    gProp->propGetDouble(in, kOfxPropTime, 0, &time);
    gProp->propGetDoubleN(in, kOfxInteractPropPixelScale, 2, ps);
    OfxParamHandle centre = probeCentre(effect);
    gParam->paramGetValueAtTime(centre, time, &cx, &cy);
    const double hx = 10 * ps[0], hy = 10 * ps[1];
    if (draw) {
        void* ctx = NULL;
        if (!gDraw || gProp->propGetPointer(in, kOfxInteractPropDrawContext, 0, &ctx) != kOfxStatOK || !ctx) return kOfxStatFailed;
        const OfxRGBAColourF red = {1, 0, 0, 1}, yellow = {1, 1, 0, 1};
        const OfxPointD box[2] = {{cx - hx, cy - hy}, {cx + hx, cy + hy}};
        const OfxPointD cross[4] = {{cx - 3 * hx, cy}, {cx + 3 * hx, cy}, {cx, cy - 3 * hy}, {cx, cy + 3 * hy}};
        gDraw->setColour((OfxDrawContextHandle)ctx, &yellow);
        gDraw->setLineWidth((OfxDrawContextHandle)ctx, 1);
        gDraw->draw((OfxDrawContextHandle)ctx, kOfxDrawPrimitiveLines, cross, 4);
        gDraw->setColour((OfxDrawContextHandle)ctx, &red);
        gDraw->draw((OfxDrawContextHandle)ctx, kOfxDrawPrimitiveRectangle, box, 2);
        return kOfxStatOK;
    }
    double pen[2] = {0, 0};
    gProp->propGetDoubleN(in, kOfxInteractPropPenPosition, 2, pen);
    if (down) {
        gProbeGrabbed = pen[0] >= cx - hx && pen[0] <= cx + hx && pen[1] >= cy - hy && pen[1] <= cy + hy;
        return gProbeGrabbed ? kOfxStatOK : kOfxStatReplyDefault;
    }
    if (!gProbeGrabbed) return kOfxStatReplyDefault;
    gParam->paramSetValueAtTime(centre, time, pen[0], pen[1]);
    if (up) gProbeGrabbed = 0;
    return kOfxStatOK;
}

static OfxStatus mainProbe(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out) {
    (void)out;
    OfxImageEffectHandle effect = (OfxImageEffectHandle)handle;
    if (!strcmp(action, kOfxActionLoad)) return load();
    if (!strcmp(action, kOfxActionDescribe)) {
        OfxPropertySetHandle props;
        const OfxStatus st = describe(effect, "Test Probe", 0);
        gEffect->getPropertySet(effect, &props);
        gProp->propSetPointer(props, kOfxImageEffectPluginPropOverlayInteractV2, 0, (void*)probeOverlay);
        return st;
    }
    if (!strcmp(action, kOfxImageEffectActionDescribeInContext)) return describeProbe(effect);
    if (!strcmp(action, kOfxActionCreateInstance)) {
        OfxPropertySetHandle props;
        gEffect->getPropertySet(effect, &props);
        gProp->propSetPointer(props, kOfxPropInstanceData, 0, calloc(1, sizeof(int)));
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxActionDestroyInstance)) {
        free(changesOf(effect));
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxActionInstanceChanged)) {
        char* type = NULL;
        char* name = NULL;
        gProp->propGetString(in, kOfxPropType, 0, &type);
        gProp->propGetString(in, kOfxPropName, 0, &name);
        int* changes = changesOf(effect);
        if (changes && type && name && !strcmp(type, kOfxTypeParameter) && !strcmp(name, "amount")) ++*changes;
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxImageEffectActionRender)) return renderProbe(effect, in);
    return kOfxStatReplyDefault;
}

#ifdef MONTAGE_TEST_OFX_GL
static OfxStatus glAttached(void);
static OfxStatus glDetached(void);
static OfxStatus renderInvertGl(OfxImageEffectHandle effect, OfxPropertySetHandle in);
static OfxStatus invertOverlay(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out);
#endif

static OfxStatus mainInvert(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out) {
    (void)out;
    OfxImageEffectHandle effect = (OfxImageEffectHandle)handle;
    if (!strcmp(action, kOfxActionLoad)) return load();
#ifdef MONTAGE_TEST_OFX_GL
    if (!strcmp(action, kOfxActionDescribe)) {
        OfxPropertySetHandle props;
        const OfxStatus st = describe(effect, "Test Invert", 0);
        gEffect->getPropertySet(effect, &props);
        gProp->propSetString(props, kOfxImageEffectPropOpenGLRenderSupported, 0, "true");
        gProp->propSetPointer(props, kOfxImageEffectPluginPropOverlayInteractV1, 0, (void*)invertOverlay);
        return st;
    }
    if (!strcmp(action, kOfxActionOpenGLContextAttached)) return glAttached();
    if (!strcmp(action, kOfxActionOpenGLContextDetached)) return glDetached();
    if (!strcmp(action, kOfxImageEffectActionRender)) {
        int enabled = 0;
        gProp->propGetInt(in, kOfxImageEffectPropOpenGLEnabled, 0, &enabled);
        if (enabled) return renderInvertGl(effect, in);
    }
#endif
    if (!strcmp(action, kOfxActionDescribe)) return describe(effect, "Test Invert", 0);
    if (!strcmp(action, kOfxImageEffectActionDescribeInContext)) return describeInContext(effect, 1);
    if (!strcmp(action, kOfxImageEffectActionRender)) return render(effect, in, 1);
    return kOfxStatReplyDefault;
}

static OfxStatus mainTemporal(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out) {
    (void)out;
    OfxImageEffectHandle effect = (OfxImageEffectHandle)handle;
    if (!strcmp(action, kOfxActionLoad)) return load();
    if (!strcmp(action, kOfxActionDescribe)) return describe(effect, "Test Temporal Average", 1);
    if (!strcmp(action, kOfxImageEffectActionDescribeInContext)) return describeInContext(effect, 0);
    if (!strcmp(action, kOfxImageEffectActionRender)) return render(effect, in, 0);
    return kOfxStatReplyDefault;
}

static OfxStatus describeColour(OfxImageEffectHandle effect) {
    OfxPropertySetHandle props;
    gEffect->getPropertySet(effect, &props);
    gProp->propSetString(props, kOfxImageEffectPropColourManagementStyle, 0, kOfxImageEffectColourManagementCore);
    gProp->propSetString(props, kOfxImageEffectPropColourManagementAvailableConfigs, 0, "ofx-native-v1.5_aces-v1.3_ocio-v2.3");
    return describe(effect, "Test Colour", 0);
}

static OfxStatus describeColourInContext(OfxImageEffectHandle effect) {
    describeInContext(effect, 0);
    OfxParamSetHandle params;
    gEffect->getParamSet(effect, &params);
    OfxPropertySetHandle p;
    gParam->paramDefine(params, kOfxParamTypeChoice, "mode", &p);
    gProp->propSetString(p, kOfxPropLabel, 0, "Mode");
    gProp->propSetString(p, kOfxParamPropChoiceOption, 0, "Report");
    gProp->propSetString(p, kOfxParamPropChoiceOption, 1, "Pass");
    gProp->propSetString(p, kOfxParamPropChoiceOption, 2, "Working");
    return kOfxStatOK;
}

static int colourMode(OfxImageEffectHandle effect, OfxTime time) {
    OfxParamSetHandle params;
    OfxParamHandle h;
    int mode = 0;
    gEffect->getParamSet(effect, &params);
    gParam->paramGetHandle(params, "mode", &h, NULL);
    gParam->paramGetValueAtTime(h, time, &mode);
    return mode;
}

static int propIs(OfxPropertySetHandle props, const char* name, const char* value) {
    char* v = NULL;
    return gProp->propGetString(props, name, 0, &v) == kOfxStatOK && v && !strcmp(v, value);
}

/* The working colourspace the host gave when asked for the output's. */
static char gWorking[64];

static OfxStatus renderColour(OfxImageEffectHandle effect, OfxPropertySetHandle in) {
    OfxTime time;
    int window[4];
    gProp->propGetDouble(in, kOfxPropTime, 0, &time);
    gProp->propGetIntN(in, kOfxImageEffectPropRenderWindow, 4, window);
    OfxImageClipHandle source, output;
    gEffect->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &source, NULL);
    gEffect->clipGetHandle(effect, kOfxImageEffectOutputClipName, &output, NULL);
    OfxPropertySetHandle outImg = NULL, srcImg = NULL, props, clipProps;
    if (gEffect->clipGetImage(output, time, NULL, &outImg) != kOfxStatOK) return kOfxStatFailed;
    if (gEffect->clipGetImage(source, time, NULL, &srcImg) != kOfxStatOK) {
        gEffect->clipReleaseImage(outImg);
        return kOfxStatFailed;
    }
    gEffect->getPropertySet(effect, &props);
    gEffect->clipGetPropertySet(source, &clipProps);
    const int core = propIs(props, kOfxImageEffectPropColourManagementStyle, kOfxImageEffectColourManagementCore);
    const int linear = propIs(clipProps, kOfxImageClipPropColourspace, kOfxColourspaceACEScg);
    const int mode = colourMode(effect, time), report = mode == 0;
    const int toldAcescct = !strcmp(gWorking, kOfxColourspaceACEScct);
    View o, s;
    viewOf(outImg, &o);
    viewOf(srcImg, &s);
    for (int y = window[1]; y < window[3]; ++y)
        for (int x = window[0]; x < window[2]; ++x) {
            const float* sp = pixel(&s, x, y);
            float* op = pixel(&o, x, y);
            if (report) op[0] = sp[0], op[1] = (float)core, op[2] = (float)linear, op[3] = 1;
            else if (mode == 2) op[0] = sp[0], op[1] = (float)toldAcescct, op[2] = (float)linear, op[3] = 1;
            else op[0] = sp[0], op[1] = sp[1], op[2] = sp[2], op[3] = sp[3];
        }
    gEffect->clipReleaseImage(srcImg);
    gEffect->clipReleaseImage(outImg);
    return kOfxStatOK;
}

static OfxStatus mainColour(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out) {
    OfxImageEffectHandle effect = (OfxImageEffectHandle)handle;
    if (!strcmp(action, kOfxActionLoad)) return load();
    if (!strcmp(action, kOfxActionDescribe)) return describeColour(effect);
    if (!strcmp(action, kOfxImageEffectActionDescribeInContext)) return describeColourInContext(effect);
    if (!strcmp(action, kOfxImageEffectActionGetClipPreferences)) {
        gProp->propSetString(out, "OfxImageClipPropPreferredColourspaces_Source", 0, kOfxColourspaceACEScg);
        gProp->propSetString(out, "OfxImageClipPropPreferredColourspaces_Source", 1, kOfxColourspaceOfxSceneLinear);
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxImageEffectActionGetOutputColourspace)) {
        char* working = NULL;
        gWorking[0] = 0;
        if (gProp->propGetString(in, kOfxImageClipPropPreferredColourspaces, 0, &working) == kOfxStatOK && working)
            snprintf(gWorking, sizeof gWorking, "%s", working);
        gProp->propSetString(out, kOfxImageClipPropColourspace, 0, colourMode(effect, 0) != 1 ? kOfxColourspaceOfxRaw : "OfxColourspace_Source");
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxImageEffectActionRender)) return renderColour(effect, in);
    return kOfxStatReplyDefault;
}


/* ---- Test Dissolve (transition), Test Generator, Test Matte (General) ---- */

static void defineClip(OfxImageEffectHandle effect, const char* name, int optional, int mask) {
    OfxPropertySetHandle clip;
    gEffect->clipDefine(effect, name, &clip);
    gProp->propSetString(clip, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    if (optional) gProp->propSetInt(clip, kOfxImageClipPropOptional, 0, 1);
    if (mask) gProp->propSetInt(clip, kOfxImageClipPropIsMask, 0, 1);
}

static double doubleParam(OfxImageEffectHandle effect, const char* name, OfxTime time) {
    OfxParamSetHandle params;
    OfxParamHandle h;
    double v = 0;
    gEffect->getParamSet(effect, &params);
    if (gParam->paramGetHandle(params, name, &h, NULL) == kOfxStatOK) gParam->paramGetValueAtTime(h, time, &v);
    return v;
}

typedef struct {
    OfxTime time;
    int window[4];
} RenderArgs;

static void renderArgs(OfxPropertySetHandle in, RenderArgs* a) {
    gProp->propGetDouble(in, kOfxPropTime, 0, &a->time);
    gProp->propGetIntN(in, kOfxImageEffectPropRenderWindow, 4, a->window);
}

static OfxPropertySetHandle imageOf(OfxImageEffectHandle effect, const char* clip, OfxTime time, View* v) {
    OfxImageClipHandle h;
    OfxPropertySetHandle img = NULL;
    if (gEffect->clipGetHandle(effect, clip, &h, NULL) != kOfxStatOK) return NULL;
    if (gEffect->clipGetImage(h, time, NULL, &img) != kOfxStatOK || !img) return NULL;
    if (!viewOf(img, v)) {
        gEffect->clipReleaseImage(img);
        return NULL;
    }
    return img;
}

static OfxStatus renderDissolve(OfxImageEffectHandle effect, OfxPropertySetHandle in) {
    RenderArgs a;
    renderArgs(in, &a);
    View o, f, t;
    OfxPropertySetHandle outImg = imageOf(effect, kOfxImageEffectOutputClipName, a.time, &o);
    OfxPropertySetHandle fromImg = imageOf(effect, kOfxImageEffectTransitionSourceFromClipName, a.time, &f);
    OfxPropertySetHandle toImg = imageOf(effect, kOfxImageEffectTransitionSourceToClipName, a.time, &t);
    OfxStatus st = kOfxStatFailed;
    if (outImg && fromImg && toImg) {
        const float k = (float)doubleParam(effect, kOfxImageEffectTransitionParamName, a.time);
        for (int y = a.window[1]; y < a.window[3]; ++y)
            for (int x = a.window[0]; x < a.window[2]; ++x) {
                const float* fp = pixel(&f, x, y);
                const float* tp = pixel(&t, x, y);
                float* op = pixel(&o, x, y);
                for (int c = 0; c < 4; ++c) op[c] = fp[c] * (1 - k) + tp[c] * k;
            }
        st = kOfxStatOK;
    }
    if (outImg) gEffect->clipReleaseImage(outImg);
    if (fromImg) gEffect->clipReleaseImage(fromImg);
    if (toImg) gEffect->clipReleaseImage(toImg);
    return st;
}

static OfxStatus mainDissolve(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out) {
    (void)out;
    OfxImageEffectHandle effect = (OfxImageEffectHandle)handle;
    if (!strcmp(action, kOfxActionLoad)) return load();
    if (!strcmp(action, kOfxActionDescribe)) return describeAs(effect, "Test Dissolve", 0, kOfxImageEffectContextTransition);
    if (!strcmp(action, kOfxImageEffectActionDescribeInContext)) {
        defineClip(effect, kOfxImageEffectTransitionSourceFromClipName, 0, 0);
        defineClip(effect, kOfxImageEffectTransitionSourceToClipName, 0, 0);
        defineClip(effect, kOfxImageEffectOutputClipName, 0, 0);
        OfxParamSetHandle params;
        OfxPropertySetHandle p;
        gEffect->getParamSet(effect, &params);
        gParam->paramDefine(params, kOfxParamTypeDouble, kOfxImageEffectTransitionParamName, &p);
        gProp->propSetString(p, kOfxPropLabel, 0, "Transition");
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxImageEffectActionRender)) return renderDissolve(effect, in);
    return kOfxStatReplyDefault;
}

static OfxStatus renderGenerator(OfxImageEffectHandle effect, OfxPropertySetHandle in) {
    RenderArgs a;
    renderArgs(in, &a);
    View o;
    OfxPropertySetHandle outImg = imageOf(effect, kOfxImageEffectOutputClipName, a.time, &o);
    if (!outImg) return kOfxStatFailed;
    OfxParamSetHandle params;
    OfxParamHandle h;
    double colour[4] = {0, 0, 0, 1};
    gEffect->getParamSet(effect, &params);
    gParam->paramGetHandle(params, "colour", &h, NULL);
    gParam->paramGetValueAtTime(h, a.time, &colour[0], &colour[1], &colour[2], &colour[3]);
    const double split = doubleParam(effect, "split", a.time);
    const int edge = o.x1 + (int)(split * (o.x2 - o.x1) + 0.5);
    for (int y = a.window[1]; y < a.window[3]; ++y)
        for (int x = a.window[0]; x < a.window[2]; ++x) {
            float* op = pixel(&o, x, y);
            const int inside = x < edge;
            /* (premultiplied) */
            for (int c = 0; c < 3; ++c) op[c] = inside ? (float)(colour[c] * colour[3]) : 0.0f;
            op[3] = inside ? (float)colour[3] : 0.0f;
        }
    gEffect->clipReleaseImage(outImg);
    return kOfxStatOK;
}

static OfxStatus mainGenerator(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out) {
    (void)out;
    OfxImageEffectHandle effect = (OfxImageEffectHandle)handle;
    if (!strcmp(action, kOfxActionLoad)) return load();
    if (!strcmp(action, kOfxActionDescribe)) return describeAs(effect, "Test Generator", 0, kOfxImageEffectContextGenerator);
    if (!strcmp(action, kOfxImageEffectActionDescribeInContext)) {
        defineClip(effect, kOfxImageEffectOutputClipName, 0, 0);
        OfxParamSetHandle params;
        OfxPropertySetHandle p;
        gEffect->getParamSet(effect, &params);
        gParam->paramDefine(params, kOfxParamTypeRGBA, "colour", &p);
        gProp->propSetString(p, kOfxPropLabel, 0, "Colour");
        gProp->propSetDouble(p, kOfxParamPropDefault, 0, 0.25);
        gProp->propSetDouble(p, kOfxParamPropDefault, 1, 0.5);
        gProp->propSetDouble(p, kOfxParamPropDefault, 2, 0.75);
        gProp->propSetDouble(p, kOfxParamPropDefault, 3, 1.0);
        gParam->paramDefine(params, kOfxParamTypeDouble, "split", &p);
        gProp->propSetString(p, kOfxPropLabel, 0, "Split");
        gProp->propSetDouble(p, kOfxParamPropDefault, 0, 0.5);
        gProp->propSetDouble(p, kOfxParamPropMin, 0, 0.0);
        gProp->propSetDouble(p, kOfxParamPropMax, 0, 1.0);
        gProp->propSetDouble(p, kOfxParamPropDisplayMin, 0, 0.0);
        gProp->propSetDouble(p, kOfxParamPropDisplayMax, 0, 1.0);
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxImageEffectActionRender)) return renderGenerator(effect, in);
    return kOfxStatReplyDefault;
}

static OfxStatus renderMatte(OfxImageEffectHandle effect, OfxPropertySetHandle in) {
    RenderArgs a;
    renderArgs(in, &a);
    View o, s, m;
    OfxPropertySetHandle outImg = imageOf(effect, kOfxImageEffectOutputClipName, a.time, &o);
    OfxPropertySetHandle srcImg = imageOf(effect, kOfxImageEffectSimpleSourceClipName, a.time, &s);
    OfxPropertySetHandle matteImg = imageOf(effect, "Matte", a.time, &m);  /* (none when it is not connected) */
    OfxStatus st = kOfxStatFailed;
    if (outImg && srcImg) {
        for (int y = a.window[1]; y < a.window[3]; ++y)
            for (int x = a.window[0]; x < a.window[2]; ++x) {
                const float* sp = pixel(&s, x, y);
                float* op = pixel(&o, x, y);
                const int in = matteImg && x >= m.x1 && x < m.x2 && y >= m.y1 && y < m.y2;
                const float k = matteImg ? (in ? pixel(&m, x, y)[3] : 0.0f) : 1.0f;
                for (int c = 0; c < 4; ++c) op[c] = sp[c] * k;
            }
        st = kOfxStatOK;
    }
    if (outImg) gEffect->clipReleaseImage(outImg);
    if (srcImg) gEffect->clipReleaseImage(srcImg);
    if (matteImg) gEffect->clipReleaseImage(matteImg);
    return st;
}

static OfxStatus mainMatte(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out) {
    (void)out;
    OfxImageEffectHandle effect = (OfxImageEffectHandle)handle;
    if (!strcmp(action, kOfxActionLoad)) return load();
    if (!strcmp(action, kOfxActionDescribe)) return describeAs(effect, "Test Matte", 0, kOfxImageEffectContextGeneral);
    if (!strcmp(action, kOfxImageEffectActionDescribeInContext)) {
        defineClip(effect, kOfxImageEffectSimpleSourceClipName, 0, 0);
        defineClip(effect, "Matte", 1, 1);
        defineClip(effect, kOfxImageEffectOutputClipName, 0, 0);
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxImageEffectActionRender)) return renderMatte(effect, in);
    return kOfxStatReplyDefault;
}

/* ---- Test Invert's OpenGL render and OpenGL on-screen controls ---- */
#ifdef MONTAGE_TEST_OFX_GL
typedef char GLchar_;
typedef unsigned (*CreateShader)(unsigned);
typedef void (*ShaderSource)(unsigned, int, const GLchar_* const*, const int*);
typedef void (*CompileShader)(unsigned);
typedef unsigned (*CreateProgram)(void);
typedef void (*AttachShader)(unsigned, unsigned);
typedef void (*LinkProgram)(unsigned);
typedef void (*UseProgram)(unsigned);
typedef int (*GetUniformLocation)(unsigned, const GLchar_*);
typedef void (*Uniform1i)(int, int);
typedef void (*Uniform1f)(int, float);
typedef void (*Uniform3f)(int, float, float, float);
typedef void (*DeleteProgram)(unsigned);
typedef void (*DeleteShader)(unsigned);
typedef void (*GetProgramiv)(unsigned, unsigned, int*);

static struct {
    CreateShader createShader;
    ShaderSource shaderSource;
    CompileShader compileShader;
    CreateProgram createProgram;
    AttachShader attachShader;
    LinkProgram linkProgram;
    UseProgram useProgram;
    GetUniformLocation uniformLocation;
    Uniform1i uniform1i;
    Uniform1f uniform1f;
    Uniform3f uniform3f;
    DeleteProgram deleteProgram;
    DeleteShader deleteShader;
    GetProgramiv getProgramiv;
    unsigned program;
} gGl;

static void* glProc(const char* name) {
#ifdef _WIN32
    void* p = (void*)wglGetProcAddress(name);
    return p ? p : (void*)GetProcAddress(GetModuleHandleA("opengl32.dll"), name);
#else
    return dlsym(RTLD_DEFAULT, name);
#endif
}

static const char* kVertex = "void main() { gl_TexCoord[0] = gl_MultiTexCoord0; gl_Position = gl_Vertex; }";
static const char* kFragment =
    "uniform sampler2D src; uniform float amount; uniform vec3 tint; uniform int mode;\n"
    "void main() {\n"
    "  vec4 s = texture2D(src, gl_TexCoord[0].st);\n"
    "  vec3 v = s.rgb + (vec3(s.a) - s.rgb - s.rgb) * amount;\n"
    "  gl_FragColor = vec4(mode == 1 ? s.rgb : v * tint, s.a);\n"
    "}\n";

static OfxStatus glAttached(void) {
#define PROC(field, type, name) if (!(gGl.field = (type)glProc(name))) return kOfxStatFailed
    PROC(createShader, CreateShader, "glCreateShader");
    PROC(shaderSource, ShaderSource, "glShaderSource");
    PROC(compileShader, CompileShader, "glCompileShader");
    PROC(createProgram, CreateProgram, "glCreateProgram");
    PROC(attachShader, AttachShader, "glAttachShader");
    PROC(linkProgram, LinkProgram, "glLinkProgram");
    PROC(useProgram, UseProgram, "glUseProgram");
    PROC(uniformLocation, GetUniformLocation, "glGetUniformLocation");
    PROC(uniform1i, Uniform1i, "glUniform1i");
    PROC(uniform1f, Uniform1f, "glUniform1f");
    PROC(uniform3f, Uniform3f, "glUniform3f");
    PROC(deleteProgram, DeleteProgram, "glDeleteProgram");
    PROC(deleteShader, DeleteShader, "glDeleteShader");
    PROC(getProgramiv, GetProgramiv, "glGetProgramiv");
#undef PROC
    if (gGl.program) return kOfxStatOK;
    const unsigned vs = gGl.createShader(0x8B31 /* GL_VERTEX_SHADER */), fs = gGl.createShader(0x8B30 /* GL_FRAGMENT_SHADER */);
    gGl.shaderSource(vs, 1, &kVertex, NULL);
    gGl.shaderSource(fs, 1, &kFragment, NULL);
    gGl.compileShader(vs);
    gGl.compileShader(fs);
    gGl.program = gGl.createProgram();
    gGl.attachShader(gGl.program, vs);
    gGl.attachShader(gGl.program, fs);
    gGl.linkProgram(gGl.program);
    gGl.deleteShader(vs);
    gGl.deleteShader(fs);
    int linked = 0;
    gGl.getProgramiv(gGl.program, 0x8B82 /* GL_LINK_STATUS */, &linked);
    if (!linked) {
        gGl.deleteProgram(gGl.program);
        gGl.program = 0;
        return kOfxStatFailed;
    }
    return kOfxStatOK;
}

static OfxStatus glDetached(void) {
    if (gGl.program && gGl.deleteProgram) gGl.deleteProgram(gGl.program);
    gGl.program = 0;
    return kOfxStatOK;
}

static OfxStatus renderInvertGl(OfxImageEffectHandle effect, OfxPropertySetHandle in) {
    OfxImageEffectOpenGLRenderSuiteV1* suite =
        (OfxImageEffectOpenGLRenderSuiteV1*)gHost->fetchSuite(gHost->host, kOfxOpenGLRenderSuite, 1);
    if (!suite || !gGl.program) return kOfxStatFailed;
    RenderArgs a;
    renderArgs(in, &a);
    OfxImageClipHandle source, output;
    gEffect->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &source, NULL);
    gEffect->clipGetHandle(effect, kOfxImageEffectOutputClipName, &output, NULL);
    OfxPropertySetHandle outTex = NULL, srcTex = NULL;
    if (suite->clipLoadTexture(output, a.time, NULL, NULL, &outTex) != kOfxStatOK) return kOfxStatFailed;
    if (suite->clipLoadTexture(source, a.time, NULL, NULL, &srcTex) != kOfxStatOK) {
        suite->clipFreeTexture(outTex);
        return kOfxStatFailed;
    }
    int srcIndex = 0, bounds[4];
    gProp->propGetInt(srcTex, kOfxImageEffectPropOpenGLTextureIndex, 0, &srcIndex);
    gProp->propGetIntN(outTex, kOfxImagePropBounds, 4, bounds);
    OfxParamSetHandle params;
    OfxParamHandle h;
    double amount = 1, tint[3] = {1, 1, 1};
    int mode = 0;
    gEffect->getParamSet(effect, &params);
    gParam->paramGetHandle(params, "amount", &h, NULL);
    gParam->paramGetValueAtTime(h, a.time, &amount);
    gParam->paramGetHandle(params, "mode", &h, NULL);
    gParam->paramGetValueAtTime(h, a.time, &mode);
    gParam->paramGetHandle(params, "tint", &h, NULL);
    gParam->paramGetValueAtTime(h, a.time, &tint[0], &tint[1], &tint[2]);
    /* (the output's framebuffer is the one bound: draw the whole of it) */
    glViewport(0, 0, bounds[2] - bounds[0], bounds[3] - bounds[1]);
    gGl.useProgram(gGl.program);
    gGl.uniform1i(gGl.uniformLocation(gGl.program, "src"), 0);
    gGl.uniform1f(gGl.uniformLocation(gGl.program, "amount"), (float)amount);
    gGl.uniform3f(gGl.uniformLocation(gGl.program, "tint"), (float)tint[0], (float)tint[1], (float)tint[2]);
    gGl.uniform1i(gGl.uniformLocation(gGl.program, "mode"), mode);
    glBindTexture(GL_TEXTURE_2D, (GLuint)srcIndex);
    glDisable(GL_BLEND);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0), glVertex2f(-1, -1);
    glTexCoord2f(1, 0), glVertex2f(1, -1);
    glTexCoord2f(1, 1), glVertex2f(1, 1);
    glTexCoord2f(0, 1), glVertex2f(-1, 1);
    glEnd();
    gGl.useProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glFinish();
    suite->clipFreeTexture(srcTex);
    suite->clipFreeTexture(outTex);
    return kOfxStatOK;
}

static OfxStatus invertOverlay(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out) {
    (void)handle, (void)in, (void)out;
    if (!strcmp(action, kOfxActionDescribe) || !strcmp(action, kOfxActionCreateInstance) || !strcmp(action, kOfxActionDestroyInstance))
        return kOfxStatOK;
    if (strcmp(action, kOfxInteractActionDraw)) return kOfxStatReplyDefault;
    glColor4f(0, 1, 0, 1);
    glBegin(GL_QUADS);
    glVertex2d(100, 100), glVertex2d(200, 100), glVertex2d(200, 200), glVertex2d(100, 200);
    glEnd();
    return kOfxStatOK;
}
#endif

static OfxPlugin gInvert = {kOfxImageEffectPluginApi, 1, "org.montage.test.invert", 1, 0, setHost, mainInvert};
static OfxPlugin gTemporal = {kOfxImageEffectPluginApi, 1, "org.montage.test.temporal", 1, 0, setHost, mainTemporal};
static OfxPlugin gProbe = {kOfxImageEffectPluginApi, 1, "org.montage.test.probe", 1, 0, setHost, mainProbe};
static OfxPlugin gColour = {kOfxImageEffectPluginApi, 1, "org.montage.test.colour", 1, 0, setHost, mainColour};

static OfxPlugin gDissolve = {kOfxImageEffectPluginApi, 1, "org.montage.test.dissolve", 1, 0, setHost, mainDissolve};
static OfxPlugin gGenerator = {kOfxImageEffectPluginApi, 1, "org.montage.test.generator", 1, 0, setHost, mainGenerator};
static OfxPlugin gMatte = {kOfxImageEffectPluginApi, 1, "org.montage.test.matte", 1, 0, setHost, mainMatte};
static OfxPlugin* gPlugins[] = {&gInvert, &gTemporal, &gProbe, &gColour, &gDissolve, &gGenerator, &gMatte};

EXPORT int OfxGetNumberOfPlugins(void) {
    return (int)(sizeof gPlugins / sizeof gPlugins[0]);
}

EXPORT OfxPlugin* OfxGetPlugin(int nth) {
    return nth >= 0 && nth < OfxGetNumberOfPlugins() ? gPlugins[nth] : NULL;
}
