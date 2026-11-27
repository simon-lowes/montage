/* A reproducer for a Mesa fault: a GLSL ES 1.00 fragment shader that reads gl_LastFragData (EXT_shader_framebuffer_fetch
 * or its non-coherent form) and writes gl_FragColor writes zero.
 *
 * Seen under llvmpipe, Mesa 25.2.8 (Ubuntu 24.04's), in a surfaceless EGL context of OpenGL ES 3.2 or 2.0, to RGBA8
 * and RGBA16F. It is why every window is black when Weston 16 blends in its shaders on a colour-managed output
 * (its GL renderer's fragment.glsl, DEF_SHADER_BLENDING: GLSL ES 1.00, `layout(noncoherent) mediump vec4
 * gl_LastFragData[gl_MaxDrawBuffers]`, then `gl_FragColor = dst`). The same blending written to gl_FragData[0], or in
 * GLSL ES 3.00 with an `inout` output, comes out right, so the fault is in how gl_LastFragData is joined to
 * gl_FragColor (which writes every draw buffer) rather than in the fetch itself.
 *
 * Each case clears a 64 by 64 target to (0.5, 0.25, 0.125, 1), draws a clear source over it ((0, 0, 0, 0): the target
 * stays), then a source of (0.1, 0, 0, 0.2) premultiplied over it, as Weston blends: the target should then be (0.5,
 * 0.2, 0.1, 1). It prints what each case gave and the driver's name and version.
 *
 * Exit status: 0 when the fault is seen (the gl_FragColor cases give zero and the others are right), 1 when it is not
 * (a fixed Mesa: Montage's CI then says so, and the MESA_EXTENSION_OVERRIDE in scripts/wayland-hdr-check.sh can go), 2
 * when it cannot run (no EGL, no OpenGL ES 3, a shader refused).
 *
 *   cc -o mesa-lastfragdata mesa-lastfragdata.c -lEGL -lGLESv2
 *   LIBGL_ALWAYS_SOFTWARE=1 EGL_PLATFORM=surfaceless ./mesa-lastfragdata
 *
 * Part of Montage's tests (GPL-3.0-or-later); written for Mesa's issue tracker, free to send there as it is. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

struct Case {
    const char* name;
    int essl3;      /* GLSL ES 3.00 (else 1.00) */
    int nonCoherent;
    int fragColor;  /* writes gl_FragColor (the fault's case) */
    const char* source;
};

static const struct Case kCases[] = {
    {"ES 1.00, non-coherent fetch, gl_FragColor", 0, 1, 1,
     "#extension GL_EXT_shader_framebuffer_fetch_non_coherent : require\nprecision highp float;\n"
     "layout(noncoherent) mediump vec4 gl_LastFragData[gl_MaxDrawBuffers];\nuniform vec4 src;\n"
     "void main() { vec4 dst = gl_LastFragData[0]; gl_FragColor = src + (1.0 - src.a) * dst; }\n"},
    {"ES 1.00, coherent fetch, gl_FragColor", 0, 0, 1,
     "#extension GL_EXT_shader_framebuffer_fetch : require\nprecision highp float;\nuniform vec4 src;\n"
     "void main() { vec4 dst = gl_LastFragData[0]; gl_FragColor = src + (1.0 - src.a) * dst; }\n"},
    {"ES 1.00, non-coherent fetch, gl_FragData[0]", 0, 1, 0,
     "#extension GL_EXT_shader_framebuffer_fetch_non_coherent : require\nprecision highp float;\n"
     "layout(noncoherent) mediump vec4 gl_LastFragData[gl_MaxDrawBuffers];\nuniform vec4 src;\n"
     "void main() { vec4 dst = gl_LastFragData[0]; gl_FragData[0] = src + (1.0 - src.a) * dst; }\n"},
    {"ES 1.00, coherent fetch, gl_FragData[0]", 0, 0, 0,
     "#extension GL_EXT_shader_framebuffer_fetch : require\nprecision highp float;\nuniform vec4 src;\n"
     "void main() { vec4 dst = gl_LastFragData[0]; gl_FragData[0] = src + (1.0 - src.a) * dst; }\n"},
    {"ES 3.00, non-coherent fetch, inout", 1, 1, 0,
     "#version 300 es\n#extension GL_EXT_shader_framebuffer_fetch_non_coherent : require\nprecision highp float;\n"
     "layout(noncoherent) inout highp vec4 o;\nuniform vec4 src;\nvoid main() { o = src + (1.0 - src.a) * o; }\n"},
    {"ES 3.00, coherent fetch, inout", 1, 0, 0,
     "#version 300 es\n#extension GL_EXT_shader_framebuffer_fetch : require\nprecision highp float;\n"
     "inout highp vec4 o;\nuniform vec4 src;\nvoid main() { o = src + (1.0 - src.a) * o; }\n"},
};

static GLuint compile(GLenum type, const char* source, const char* name) {
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &source, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "%s: the shader is refused: %s\n", name, log);
        return 0;
    }
    return s;
}

/* Draws the case into a fresh target of the format and reads its middle back; 0 on a failure to set it up. */
static int run(const struct Case* c, GLenum format, float out[4]) {
    GLuint tex = 0, fb = 0, vb = 0, program = 0, vs, fs;
    const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    PFNGLFRAMEBUFFERFETCHBARRIEREXTPROC barrier = (PFNGLFRAMEBUFFERFETCHBARRIEREXTPROC)eglGetProcAddress("glFramebufferFetchBarrierEXT");
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, format, 64, 64);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return 0;
    glViewport(0, 0, 64, 64);
    glDisable(GL_BLEND);
    glClearColor(0.5f, 0.25f, 0.125f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    vs = compile(GL_VERTEX_SHADER,
                 c->essl3 ? "#version 300 es\nin vec2 p;\nvoid main() { gl_Position = vec4(p, 0.0, 1.0); }\n"
                          : "attribute vec2 p;\nvoid main() { gl_Position = vec4(p, 0.0, 1.0); }\n",
                 c->name);
    fs = compile(GL_FRAGMENT_SHADER, c->source, c->name);
    if (!vs || !fs) return 0;
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "p");
    glLinkProgram(program);
    {
        GLint ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            fprintf(stderr, "%s: the program is not linked\n", c->name);
            return 0;
        }
    }
    glUseProgram(program);
    glGenBuffers(1, &vb);
    glBindBuffer(GL_ARRAY_BUFFER, vb);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glUniform4f(glGetUniformLocation(program, "src"), 0.0f, 0.0f, 0.0f, 0.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (c->nonCoherent && barrier) barrier();  /* (as Weston does between paint nodes) */
    glUniform4f(glGetUniformLocation(program, "src"), 0.1f, 0.0f, 0.0f, 0.2f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (format == GL_RGBA16F) {
        glReadPixels(32, 32, 1, 1, GL_RGBA, GL_FLOAT, out);
    } else {
        unsigned char px[4] = {0, 0, 0, 0};
        int i;
        glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        for (i = 0; i < 4; ++i) out[i] = px[i] / 255.0f;
    }
    glDeleteProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glDeleteBuffers(1, &vb);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &tex);
    return glGetError() == GL_NO_ERROR;
}

int main(void) {
    static const float want[4] = {0.5f, 0.2f, 0.1f, 1.0f};
    PFNEGLGETPLATFORMDISPLAYEXTPROC platformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay display = platformDisplay ? platformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
    EGLint major = 0, minor = 0, n = 0;
    EGLConfig config = 0;
    const EGLint configAttributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    const EGLint contextAttributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    EGLContext context;
    const char* extensions;
    int faultSeen = 1, othersRight = 1, ran = 0;
    size_t i;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
        fprintf(stderr, "No surfaceless EGL display\n");
        return 2;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    eglChooseConfig(display, configAttributes, &config, 1, &n);
    context = eglCreateContext(display, n > 0 ? config : (EGLConfig)0, EGL_NO_CONTEXT, contextAttributes);
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
        fprintf(stderr, "No OpenGL ES 3 context\n");
        return 2;
    }
    extensions = (const char*)glGetString(GL_EXTENSIONS);
    printf("%s\n%s\n", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));
    if (!extensions || !strstr(extensions, "GL_EXT_shader_framebuffer_fetch_non_coherent") || !strstr(extensions, "GL_EXT_shader_framebuffer_fetch ")) {
        fprintf(stderr, "This driver has no framebuffer fetch\n");
        return 2;
    }
    for (i = 0; i < sizeof kCases / sizeof kCases[0]; ++i) {
        const struct Case* c = &kCases[i];
        const GLenum formats[2] = {GL_RGBA8, GL_RGBA16F};
        int f;
        for (f = 0; f < 2; ++f) {
            float got[4] = {-1, -1, -1, -1};
            int k, right = 1, zero = 1;
            if (!run(c, formats[f], got)) return 2;
            for (k = 0; k < 4; ++k) {
                right = right && fabsf(got[k] - want[k]) < 0.01f;
                zero = zero && got[k] == 0.0f;
            }
            printf("%-46s %-7s %.4f %.4f %.4f %.4f  %s\n", c->name, f ? "RGBA16F" : "RGBA8", got[0], got[1], got[2], got[3],
                   right ? "right" : zero ? "ZERO" : "wrong");
            if (c->fragColor) faultSeen = faultSeen && zero;
            else othersRight = othersRight && right;
            ++ran;
        }
    }
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglTerminate(display);
    printf("(want %.4f %.4f %.4f %.4f in each)\n", want[0], want[1], want[2], want[3]);
    if (ran == 0) return 2;
    if (faultSeen && othersRight) {
        printf("The fault is seen: reading gl_LastFragData and writing gl_FragColor in GLSL ES 1.00 gives zero.\n");
        return 0;
    }
    printf("The fault is not seen%s.\n", othersRight ? "" : " as described (see the cases above)");
    return 1;
}
