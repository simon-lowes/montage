/* Montage's test OpenFX plugins, built as an .ofx.bundle:
 *  - Test Invert: inverts the (premultiplied) picture by Amount, then multiplies by Tint; Mode "Pass" leaves it alone.
 *  - Test Temporal Average: the average of the frames before, at and after the one rendered (temporal clip access).
 * With MONTAGE_TEST_OFX_CRASH defined it instead crashes while describing itself, for the scanner's blocklist. */
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#endif

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include "ofxProperty.h"

#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

static OfxHost* gHost;
static OfxPropertySuiteV1* gProp;
static OfxImageEffectSuiteV1* gEffect;
static OfxParameterSuiteV1* gParam;

static void setHost(OfxHost* h) {
    gHost = h;
}

static OfxStatus load(void) {
    gProp = (OfxPropertySuiteV1*)gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1);
    gEffect = (OfxImageEffectSuiteV1*)gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1);
    gParam = (OfxParameterSuiteV1*)gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1);
    return gProp && gEffect && gParam ? kOfxStatOK : kOfxStatErrMissingHostFeature;
}

static OfxStatus describe(OfxImageEffectHandle effect, const char* label, int temporal) {
#ifdef MONTAGE_TEST_OFX_CRASH
    (void)effect, (void)label, (void)temporal;
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
    gProp->propSetString(props, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
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

static OfxStatus mainInvert(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out) {
    (void)out;
    OfxImageEffectHandle effect = (OfxImageEffectHandle)handle;
    if (!strcmp(action, kOfxActionLoad)) return load();
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

static OfxPlugin gInvert = {kOfxImageEffectPluginApi, 1, "org.montage.test.invert", 1, 0, setHost, mainInvert};
static OfxPlugin gTemporal = {kOfxImageEffectPluginApi, 1, "org.montage.test.temporal", 1, 0, setHost, mainTemporal};

EXPORT int OfxGetNumberOfPlugins(void) {
    return 2;
}

EXPORT OfxPlugin* OfxGetPlugin(int nth) {
    return nth == 0 ? &gInvert : nth == 1 ? &gTemporal : NULL;
}
