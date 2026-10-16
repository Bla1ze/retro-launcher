#include "CrtFilter.h"

#include "Library.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using Library::log;

namespace CrtFilter {
namespace {

// Three passes, all in SDL's renderer context:
//   1. linearize: the shown part of the frame, at the game's own size, to
//      linear light stored as its square root (8 bits keep the darks that way);
//      done once per new frame instead of in every tap of the CRT shader;
//   2. crt: the CRT shader, into an offscreen picture no bigger than the work
//      budget allows (or straight to the screen when it fits);
//   3. present: that picture stretched onto the screen, turned like the plain
//      copy would be.
// The cost is per drawn pixel, so a 4K playfield is 4x a 1080p backglass. When
// the screen can't keep up, the offscreen picture shrinks (never below about
// three pixels per game line, which scanlines need).

// ------------------------------------------------------------------ shaders

const char* kVertex = R"(
attribute vec2 aPos;
attribute vec2 aUV;
varying vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

const char* kPrecision = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
varying vec2 vUV;
)";

// Pass 1. uSrcOrigin/uSrcSize: the shown part of the frame (blank edge columns
// cropped), in frame pixels. XRGB8888 rows hold B, G, R, X (uSwap).
const char* kLinearize = R"(
uniform sampler2D uTex;
uniform vec2 uTexSize;
uniform vec2 uSrcOrigin;
uniform vec2 uSrcSize;
uniform float uSwap;
float ToLinear1(float c) { return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
void main() {
    vec3 c = texture2D(uTex, (vUV * uSrcSize + uSrcOrigin) / uTexSize).rgb;
    if (uSwap > 0.5) c = c.bgr;
    gl_FragColor = vec4(sqrt(vec3(ToLinear1(c.r), ToLinear1(c.g), ToLinear1(c.b))), 1.0);
}
)";

// Pass 2's shared part: uLin is pass 1's output, uSrcSize its size; lin() takes
// a position in source pixels and gives linear light. uOutSize: the size being
// drawn, upright.
const char* kCrtHead = R"(
uniform sampler2D uLin;
uniform vec2 uSrcSize;
uniform vec2 uOutSize;
vec3 lin(vec2 px) { vec3 t = texture2D(uLin, px / uSrcSize).rgb; return t * t; }
float ToSrgb1(float c) { return (c < 0.0031308 ? c * 12.92 : 1.055 * pow(c, 0.41666) - 0.055); }
vec3 ToSrgb(vec3 c) { return vec3(ToSrgb1(c.r), ToSrgb1(c.g), ToSrgb1(c.b)); }
)";

// crt-lottes, the original:
//   PUBLIC DOMAIN CRT STYLED SCAN-LINE SHADER
//     by Timothy Lottes
//   This is more along the style of a really good CGA arcade monitor.
//   With RGB inputs instead of NTSC.
//   Please take and use, change, or whatever.
// From libretro's glsl-shaders (crt/shaders/crt-lottes.glsl) with its default
// parameters, the stretched VGA shadow mask (3) and bloom on. Same math,
// rearranged for speed: the picture's 3 lines and the bloom's 5 read the same
// texels, so the 31 are read once (the original read 42); the Gaussian weights
// depend only on the distance to the texel grid, so the 20 are computed once
// (it computed 50); its kernel shape is 2.0, so pow(|x|, shape) is x*x; and the
// input arrives already linear (pass 1). Taps beyond the picture are black.
const char* kLottes = R"(
const float hardScan = -8.0;
const float hardPix = -3.0;
const float warpX = 0.031;
const float warpY = 0.041;
const float maskDark = 0.5;
const float maskLight = 1.5;
const float hardBloomPix = -1.5;
const float hardBloomScan = -2.0;
const float bloomAmount = 0.15;

vec2 g_base;
vec3 T(float i, float j) {
    vec2 p = g_base + vec2(i, j) + vec2(0.5);
    if (p.x < 0.0 || p.y < 0.0 || p.x > uSrcSize.x || p.y > uSrcSize.y) return vec3(0.0);
    return lin(p);
}
float G(float x, float scale) { return exp2(scale * x * x); }
// Distortion of scanlines, and end of screen alpha.
vec2 Warp(vec2 pos) {
    pos = pos * 2.0 - 1.0;
    pos *= vec2(1.0 + (pos.y * pos.y) * warpX, 1.0 + (pos.x * pos.x) * warpY);
    return pos * 0.5 + 0.5;
}
// Stretched VGA style shadow mask.
vec3 Mask(vec2 pos) {
    vec3 mask = vec3(maskDark, maskDark, maskDark);
    pos.x += pos.y * 3.0;
    pos.x = fract(pos.x * 0.166666666);
    if (pos.x < 0.333) mask.r = maskLight;
    else if (pos.x < 0.666) mask.g = maskLight;
    else mask.b = maskLight;
    return mask;
}
void main() {
    vec2 pos = Warp(vUV);
    vec2 P = pos * uSrcSize;
    g_base = floor(P);
    vec2 d = -((P - g_base) - vec2(0.5));  // Dist()

    // Horizontal weights for taps -3..3 (picture: hardPix, bloom: hardBloomPix).
    float p2m = G(d.x - 2.0, hardPix), p1m = G(d.x - 1.0, hardPix), p0 = G(d.x, hardPix);
    float p1 = G(d.x + 1.0, hardPix), p2 = G(d.x + 2.0, hardPix);
    float b3m = G(d.x - 3.0, hardBloomPix), b2m = G(d.x - 2.0, hardBloomPix), b1m = G(d.x - 1.0, hardBloomPix);
    float b0 = G(d.x, hardBloomPix), b1 = G(d.x + 1.0, hardBloomPix), b2 = G(d.x + 2.0, hardBloomPix);
    float b3 = G(d.x + 3.0, hardBloomPix);
    float s3 = p1m + p0 + p1, s5 = p2m + s3 + p2;
    float t5 = b2m + b1m + b0 + b1 + b2, t7 = b3m + t5 + b3;

    // Lines -1, 0, 1: seven taps each, for the picture and the bloom.
    vec3 a3 = T(-3.0, -1.0), a2 = T(-2.0, -1.0), a1 = T(-1.0, -1.0), a0 = T(0.0, -1.0);
    vec3 a4 = T(1.0, -1.0), a5 = T(2.0, -1.0), a6 = T(3.0, -1.0);
    vec3 c3 = T(-3.0, 0.0), c2 = T(-2.0, 0.0), c1 = T(-1.0, 0.0), c0 = T(0.0, 0.0);
    vec3 c4 = T(1.0, 0.0), c5 = T(2.0, 0.0), c6 = T(3.0, 0.0);
    vec3 e3 = T(-3.0, 1.0), e2 = T(-2.0, 1.0), e1 = T(-1.0, 1.0), e0 = T(0.0, 1.0);
    vec3 e4 = T(1.0, 1.0), e5 = T(2.0, 1.0), e6 = T(3.0, 1.0);

    // Tri(): Horz3 above and below, Horz5 on the line, by scanline weight.
    vec3 tri = (a1 * p1m + a0 * p0 + a4 * p1) / s3 * G(d.y - 1.0, hardScan) +
               (c2 * p2m + c1 * p1m + c0 * p0 + c4 * p1 + c5 * p2) / s5 * G(d.y, hardScan) +
               (e1 * p1m + e0 * p0 + e4 * p1) / s3 * G(d.y + 1.0, hardScan);

    // Bloom(): Horz5 two lines out, Horz7 on the three in the middle.
    vec3 bloom =
        (T(-2.0, -2.0) * b2m + T(-1.0, -2.0) * b1m + T(0.0, -2.0) * b0 + T(1.0, -2.0) * b1 + T(2.0, -2.0) * b2) / t5 *
            G(d.y - 2.0, hardBloomScan) +
        (a3 * b3m + a2 * b2m + a1 * b1m + a0 * b0 + a4 * b1 + a5 * b2 + a6 * b3) / t7 * G(d.y - 1.0, hardBloomScan) +
        (c3 * b3m + c2 * b2m + c1 * b1m + c0 * b0 + c4 * b1 + c5 * b2 + c6 * b3) / t7 * G(d.y, hardBloomScan) +
        (e3 * b3m + e2 * b2m + e1 * b1m + e0 * b0 + e4 * b1 + e5 * b2 + e6 * b3) / t7 * G(d.y + 1.0, hardBloomScan) +
        (T(-2.0, 2.0) * b2m + T(-1.0, 2.0) * b1m + T(0.0, 2.0) * b0 + T(1.0, 2.0) * b1 + T(2.0, 2.0) * b2) / t5 *
            G(d.y + 2.0, hardBloomScan);

    vec3 outColor = (tri + bloom * bloomAmount) * Mask(gl_FragCoord.xy * 1.000001);
    if (pos.x <= 0.0001 || pos.x >= 0.9999 || pos.y <= 0.0001 || pos.y >= 0.9999) outColor = vec3(0.0);
    gl_FragColor = vec4(ToSrgb(outColor), 1.0);
}
)";

// crt-lottes-fast:
//   [CRTS] PUBLIC DOMAIN CRT-STYLED SCALAR - 20180120b
//     by Timothy Lottes
//     https://www.shadertoy.com/view/MtSfRK
//     adapted for RetroArch by hunterk
// From libretro's glsl-shaders (crt/shaders/crt-lottes-fast.glsl) with its
// default parameters: the 8-tap filter, warp, tone mapping, mask type 1. (Its
// contrast and saturation steps are no-ops at those defaults.) The input
// arrives already linear (pass 1; its CRT_GAMMA is 2.4, the same curve).
const char* kLottesFast = R"(
const float SCANLINE_THINNESS = 0.5;
const float SCAN_BLUR = 2.5;
const float MASK_INTENSITY = 0.54;
const float CURVATURE = 0.02;
const float CORNER = 3.0;

// uv: 0-1 over the source, at texel centers.
vec3 CrtsFetch(vec2 uv) { return lin(clamp(uv * uSrcSize, vec2(0.5), uSrcSize - vec2(0.5))); }

vec4 CrtsTone(float contrast, float saturation, float thin, float mask) {
    mask = 0.5 + mask * 0.5;
    vec4 ret;
    float midOut = 0.18 / ((1.5 - thin) * (0.5 * mask + 0.5));
    float pMidIn = pow(0.18, contrast);
    ret.x = contrast;
    ret.y = ((-pMidIn) + midOut) / ((1.0 - pMidIn) * midOut);
    ret.z = ((-pMidIn) * midOut + pMidIn) / (midOut * (-pMidIn) + midOut);
    ret.w = contrast + saturation;
    return ret;
}
vec3 CrtsMask(vec2 pos, float dark) {
    vec3 m = vec3(1.0, 1.0, 1.0);
    float x = fract(pos.x * (1.0 / 3.0));
    if (x < (1.0 / 3.0)) m.r = dark;
    else if (x < (2.0 / 3.0)) m.g = dark;
    else m.b = dark;
    return m;
}
vec3 CrtsFilter(vec2 ipos, vec2 halfInputSize, vec2 rcpInputSize, vec2 twoDivOutputSize, float inputHeight,
                vec2 warp, float thin, float blur, float mask, vec4 tone) {
    vec2 pos = ipos * twoDivOutputSize - vec2(1.0, 1.0);
    pos *= vec2(1.0 + (pos.y * pos.y) * warp.x, 1.0 + (pos.x * pos.x) * warp.y);
    float vin = (1.0 - ((1.0 - clamp(pos.x * pos.x, 0.0, 1.0)) * (1.0 - clamp(pos.y * pos.y, 0.0, 1.0)))) *
                (0.998 + (0.001 * CORNER));
    vin = clamp((-vin) * inputHeight + inputHeight, 0.0, 1.0);
    pos = pos * halfInputSize + halfInputSize;
    float y0 = floor(pos.y - 0.5) + 0.5;
    float x0 = floor(pos.x - 1.5) + 0.5;
    vec2 p = vec2(x0 * rcpInputSize.x, y0 * rcpInputSize.y);
    vec3 colA0 = CrtsFetch(p);
    p.x += rcpInputSize.x;
    vec3 colA1 = CrtsFetch(p);
    p.x += rcpInputSize.x;
    vec3 colA2 = CrtsFetch(p);
    p.x += rcpInputSize.x;
    vec3 colA3 = CrtsFetch(p);
    p.y += rcpInputSize.y;
    vec3 colB3 = CrtsFetch(p);
    p.x -= rcpInputSize.x;
    vec3 colB2 = CrtsFetch(p);
    p.x -= rcpInputSize.x;
    vec3 colB1 = CrtsFetch(p);
    p.x -= rcpInputSize.x;
    vec3 colB0 = CrtsFetch(p);
    float off = pos.y - y0;
    float pi2 = 6.28318530717958;
    float hlf = 0.5;
    float scanA = cos(min(0.5, off * thin) * pi2) * hlf + hlf;
    float scanB = cos(min(0.5, (-off) * thin + thin) * pi2) * hlf + hlf;
    float off0 = pos.x - x0;
    float off1 = off0 - 1.0;
    float off2 = off0 - 2.0;
    float off3 = off0 - 3.0;
    float pix0 = exp2(blur * off0 * off0);
    float pix1 = exp2(blur * off1 * off1);
    float pix2 = exp2(blur * off2 * off2);
    float pix3 = exp2(blur * off3 * off3);
    float pixT = (1.0 / (pix0 + pix1 + pix2 + pix3)) * vin;
    scanA *= pixT;
    scanB *= pixT;
    vec3 color = (colA0 * pix0 + colA1 * pix1 + colA2 * pix2 + colA3 * pix3) * scanA +
                 (colB0 * pix0 + colB1 * pix1 + colB2 * pix2 + colB3 * pix3) * scanB;
    color *= CrtsMask(ipos, mask);
    float peak = max(1.0 / (256.0 * 65536.0), max(color.r, max(color.g, color.b)));
    vec3 ratio = color * (1.0 / peak);
    peak = peak * (1.0 / (peak * tone.y + tone.z));
    return ratio * peak;
}
void main() {
    vec2 warp = vec2(CURVATURE, (3.0 / 4.0) * CURVATURE);  // assumes 4:3
    float thin = 0.5 + 0.5 * SCANLINE_THINNESS;
    float mask = 1.0 - MASK_INTENSITY;
    vec3 c = CrtsFilter(vUV * uOutSize, uSrcSize * 0.5, 1.0 / uSrcSize, 2.0 / uOutSize, uSrcSize.y,
                        warp, thin, -SCAN_BLUR, mask, CrtsTone(1.0, 0.0, thin, mask));
    gl_FragColor = vec4(ToSrgb(c), 1.0);
}
)";

// Pass 3: the offscreen picture onto the screen, smoothed.
const char* kPresent = R"(
uniform sampler2D uTex;
void main() { gl_FragColor = vec4(texture2D(uTex, vUV).rgb, 1.0); }
)";

// ------------------------------------------------------------------- GL

typedef unsigned int GLuintT;
typedef int GLintT;
enum : unsigned {
    kGL_FRAGMENT_SHADER = 0x8B30, kGL_VERTEX_SHADER = 0x8B31, kGL_COMPILE_STATUS = 0x8B81,
    kGL_LINK_STATUS = 0x8B82, kGL_CURRENT_PROGRAM = 0x8B8D, kGL_ARRAY_BUFFER = 0x8892,
    kGL_ARRAY_BUFFER_BINDING = 0x8894, kGL_TEXTURE_BINDING_2D = 0x8069, kGL_ACTIVE_TEXTURE = 0x84E0,
    kGL_TEXTURE0 = 0x84C0, kGL_BLEND = 0x0BE2, kGL_SCISSOR_TEST = 0x0C11, kGL_VIEWPORT = 0x0BA2,
    kGL_UNPACK_ALIGNMENT = 0x0CF5, kGL_VERTEX_ATTRIB_ARRAY_ENABLED = 0x8622, kGL_TEXTURE_2D = 0x0DE1,
    kGL_RGBA = 0x1908, kGL_RGB = 0x1907, kGL_UNSIGNED_BYTE = 0x1401, kGL_UNSIGNED_SHORT_5_6_5 = 0x8363,
    kGL_TEXTURE_MIN_FILTER = 0x2801, kGL_TEXTURE_MAG_FILTER = 0x2800, kGL_NEAREST = 0x2600, kGL_LINEAR = 0x2601,
    kGL_TEXTURE_WRAP_S = 0x2802, kGL_TEXTURE_WRAP_T = 0x2803, kGL_CLAMP_TO_EDGE = 0x812F,
    kGL_TRIANGLE_STRIP = 0x0005, kGL_FLOAT = 0x1406, kGL_SHADING_LANGUAGE_VERSION = 0x8B8C,
    kGL_FRAMEBUFFER = 0x8D40, kGL_FRAMEBUFFER_BINDING = 0x8CA6, kGL_COLOR_ATTACHMENT0 = 0x8CE0,
    kGL_FRAMEBUFFER_COMPLETE = 0x8CD5
};

struct Fns {
    GLuintT (*CreateShader)(unsigned);
    void (*ShaderSource)(GLuintT, int, const char* const*, const int*);
    void (*CompileShader)(GLuintT);
    void (*GetShaderiv)(GLuintT, unsigned, GLintT*);
    void (*GetShaderInfoLog)(GLuintT, int, int*, char*);
    void (*DeleteShader)(GLuintT);
    GLuintT (*CreateProgram)();
    void (*AttachShader)(GLuintT, GLuintT);
    void (*BindAttribLocation)(GLuintT, GLuintT, const char*);
    void (*LinkProgram)(GLuintT);
    void (*GetProgramiv)(GLuintT, unsigned, GLintT*);
    void (*GetProgramInfoLog)(GLuintT, int, int*, char*);
    void (*DeleteProgram)(GLuintT);
    void (*UseProgram)(GLuintT);
    GLintT (*GetUniformLocation)(GLuintT, const char*);
    void (*Uniform1i)(GLintT, GLintT);
    void (*Uniform1f)(GLintT, float);
    void (*Uniform2f)(GLintT, float, float);
    void (*GenTextures)(int, GLuintT*);
    void (*DeleteTextures)(int, const GLuintT*);
    void (*BindTexture)(unsigned, GLuintT);
    void (*ActiveTexture)(unsigned);
    void (*TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
    void (*TexSubImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
    void (*TexParameteri)(unsigned, unsigned, int);
    void (*PixelStorei)(unsigned, int);
    void (*GetIntegerv)(unsigned, GLintT*);
    unsigned char (*IsEnabled)(unsigned);
    void (*Enable)(unsigned);
    void (*Disable)(unsigned);
    void (*Viewport)(int, int, int, int);
    void (*BindBuffer)(unsigned, GLuintT);
    void (*GetVertexAttribiv)(GLuintT, unsigned, GLintT*);
    void (*EnableVertexAttribArray)(GLuintT);
    void (*DisableVertexAttribArray)(GLuintT);
    void (*VertexAttribPointer)(GLuintT, int, unsigned, unsigned char, int, const void*);
    void (*DrawArrays)(unsigned, int, int);
    void (*GenFramebuffers)(int, GLuintT*);
    void (*DeleteFramebuffers)(int, const GLuintT*);
    void (*BindFramebuffer)(unsigned, GLuintT);
    void (*FramebufferTexture2D)(unsigned, unsigned, unsigned, GLuintT, int);
    unsigned (*CheckFramebufferStatus)(unsigned);
    const unsigned char* (*GetString)(unsigned);
    unsigned (*GetError)();
} gl{};

template <typename F> bool load(F& fn, const char* name) {
    fn = (F)SDL_GL_GetProcAddress(name);
    if (!fn) log("crt: %s missing", name);
    return fn != nullptr;
}

bool loadFns() {
    return load(gl.CreateShader, "glCreateShader") & load(gl.ShaderSource, "glShaderSource") &
           load(gl.CompileShader, "glCompileShader") & load(gl.GetShaderiv, "glGetShaderiv") &
           load(gl.GetShaderInfoLog, "glGetShaderInfoLog") & load(gl.DeleteShader, "glDeleteShader") &
           load(gl.CreateProgram, "glCreateProgram") & load(gl.AttachShader, "glAttachShader") &
           load(gl.BindAttribLocation, "glBindAttribLocation") & load(gl.LinkProgram, "glLinkProgram") &
           load(gl.GetProgramiv, "glGetProgramiv") & load(gl.GetProgramInfoLog, "glGetProgramInfoLog") &
           load(gl.DeleteProgram, "glDeleteProgram") & load(gl.UseProgram, "glUseProgram") &
           load(gl.GetUniformLocation, "glGetUniformLocation") & load(gl.Uniform1i, "glUniform1i") &
           load(gl.Uniform1f, "glUniform1f") & load(gl.Uniform2f, "glUniform2f") &
           load(gl.GenTextures, "glGenTextures") & load(gl.DeleteTextures, "glDeleteTextures") &
           load(gl.BindTexture, "glBindTexture") & load(gl.ActiveTexture, "glActiveTexture") &
           load(gl.TexImage2D, "glTexImage2D") & load(gl.TexSubImage2D, "glTexSubImage2D") &
           load(gl.TexParameteri, "glTexParameteri") & load(gl.PixelStorei, "glPixelStorei") &
           load(gl.GetIntegerv, "glGetIntegerv") & load(gl.IsEnabled, "glIsEnabled") &
           load(gl.Enable, "glEnable") & load(gl.Disable, "glDisable") & load(gl.Viewport, "glViewport") &
           load(gl.BindBuffer, "glBindBuffer") & load(gl.GetVertexAttribiv, "glGetVertexAttribiv") &
           load(gl.EnableVertexAttribArray, "glEnableVertexAttribArray") &
           load(gl.DisableVertexAttribArray, "glDisableVertexAttribArray") &
           load(gl.VertexAttribPointer, "glVertexAttribPointer") & load(gl.DrawArrays, "glDrawArrays") &
           load(gl.GenFramebuffers, "glGenFramebuffers") & load(gl.DeleteFramebuffers, "glDeleteFramebuffers") &
           load(gl.BindFramebuffer, "glBindFramebuffer") & load(gl.FramebufferTexture2D, "glFramebufferTexture2D") &
           load(gl.CheckFramebufferStatus, "glCheckFramebufferStatus") & load(gl.GetString, "glGetString") &
           load(gl.GetError, "glGetError");
}

GLuintT compile(unsigned type, const std::vector<const char*>& parts, const char* what) {
    GLuintT s = gl.CreateShader(type);
    gl.ShaderSource(s, (int)parts.size(), parts.data(), nullptr);
    gl.CompileShader(s);
    GLintT ok = 0;
    gl.GetShaderiv(s, kGL_COMPILE_STATUS, &ok);
    if (!ok) {
        char msg[1024] = {};
        gl.GetShaderInfoLog(s, sizeof msg, nullptr, msg);
        log("crt: %s shader didn't compile: %s", what, msg);
        gl.DeleteShader(s);
        return 0;
    }
    return s;
}

struct Program {
    GLuintT id = 0;
    GLintT uTex = -1, uTexSize = -1, uSrcOrigin = -1, uSrcSize = -1, uOutSize = -1, uSwap = -1, uLin = -1;
};

bool link(Program& p, GLuintT vs, const std::vector<const char*>& fragment, const char* what) {
    GLuintT fs = compile(kGL_FRAGMENT_SHADER, fragment, what);
    if (!fs) return false;
    p.id = gl.CreateProgram();
    gl.AttachShader(p.id, vs);
    gl.AttachShader(p.id, fs);
    gl.BindAttribLocation(p.id, 0, "aPos");
    gl.BindAttribLocation(p.id, 1, "aUV");
    gl.LinkProgram(p.id);
    gl.DeleteShader(fs);
    GLintT ok = 0;
    gl.GetProgramiv(p.id, kGL_LINK_STATUS, &ok);
    if (!ok) {
        char msg[1024] = {};
        gl.GetProgramInfoLog(p.id, sizeof msg, nullptr, msg);
        log("crt: %s shader didn't link: %s", what, msg);
        gl.DeleteProgram(p.id);
        p.id = 0;
        return false;
    }
    p.uTex = gl.GetUniformLocation(p.id, "uTex");
    p.uTexSize = gl.GetUniformLocation(p.id, "uTexSize");
    p.uSrcOrigin = gl.GetUniformLocation(p.id, "uSrcOrigin");
    p.uSrcSize = gl.GetUniformLocation(p.id, "uSrcSize");
    p.uOutSize = gl.GetUniformLocation(p.id, "uOutSize");
    p.uSwap = gl.GetUniformLocation(p.id, "uSwap");
    p.uLin = gl.GetUniformLocation(p.id, "uLin");
    gl.UseProgram(p.id);
    if (p.uTex >= 0) gl.Uniform1i(p.uTex, 0);
    if (p.uLin >= 0) gl.Uniform1i(p.uLin, 0);
    return true;
}

// A texture with a framebuffer drawing into it.
struct Target {
    GLuintT tex = 0, fbo = 0;
    int w = 0, h = 0;
};

bool resize(Target& t, int w, int h, unsigned filter, const char* what) {
    if (t.tex && t.w == w && t.h == h) return true;
    if (!t.tex) gl.GenTextures(1, &t.tex);
    if (!t.fbo) gl.GenFramebuffers(1, &t.fbo);
    gl.BindTexture(kGL_TEXTURE_2D, t.tex);
    gl.TexImage2D(kGL_TEXTURE_2D, 0, (int)kGL_RGBA, w, h, 0, kGL_RGBA, kGL_UNSIGNED_BYTE, nullptr);
    gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_MIN_FILTER, (int)filter);
    gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_MAG_FILTER, (int)filter);
    gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_WRAP_S, kGL_CLAMP_TO_EDGE);
    gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_WRAP_T, kGL_CLAMP_TO_EDGE);
    gl.BindFramebuffer(kGL_FRAMEBUFFER, t.fbo);
    gl.FramebufferTexture2D(kGL_FRAMEBUFFER, kGL_COLOR_ATTACHMENT0, kGL_TEXTURE_2D, t.tex, 0);
    const unsigned status = gl.CheckFramebufferStatus(kGL_FRAMEBUFFER);
    t.w = w;
    t.h = h;
    log("crt: %s %dx%d: %s", what, w, h, status == kGL_FRAMEBUFFER_COMPLETE ? "ok" : "INCOMPLETE");
    return status == kGL_FRAMEBUFFER_COMPLETE;
}

void quad(const float* pos, const float* uv) {
    gl.VertexAttribPointer(0, 2, kGL_FLOAT, 0, 0, pos);
    gl.VertexAttribPointer(1, 2, kGL_FLOAT, 0, 0, uv);
    gl.DrawArrays(kGL_TRIANGLE_STRIP, 0, 4);
}

// A whole offscreen target: its first row is the picture's top.
const float kFullPos[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
const float kFullUV[8] = {0, 0, 1, 0, 0, 1, 1, 1};

// ------------------------------------------------------------------ state

Kind g_kind = Kind::Off;
bool g_ready = false, g_failed = false;
Program g_linProg, g_crtProg, g_presentProg;
GLuintT g_frameTex = 0;
Target g_lin, g_out;
// The latest frame, packed rows: 4 bytes a pixel (B, G, R, X) or RGB565.
std::vector<uint8_t> g_frame;
unsigned g_w = 0, g_h = 0;
bool g_wide = true;  // 32-bit
bool g_dirty = false;
unsigned g_texW = 0, g_texH = 0;
bool g_texWide = true;
SDL_Rect g_linSrc{0, 0, 0, 0};  // what g_lin holds
bool g_errorLogged = false;

// The work budget: the offscreen picture's share of the screen-sized one, in
// pixels (1920x1080 to start), and the frame-rate watch that lowers it.
double g_budget = 1920.0 * 1080.0;
double g_refresh = 60.0;
double g_lastFull = 1.0;  // the last picture's full size, in pixels
Uint32 g_watchStart = 0;
unsigned g_watchFrames = 0;
int g_watchRounds = 0;

bool init() {
    g_failed = true;
    if (!SDL_GL_GetCurrentContext()) { log("crt: no GL context (software renderer), filter off"); return false; }
    if (!loadFns()) return false;
    const unsigned char* ver = gl.GetString(kGL_SHADING_LANGUAGE_VERSION);
    log("crt: %s, GLSL %s", g_kind == Kind::Lottes ? "Lottes" : "Lottes (fast)", ver ? (const char*)ver : "?");
    GLintT prevProg = 0;
    gl.GetIntegerv(kGL_CURRENT_PROGRAM, &prevProg);
    GLuintT vs = compile(kGL_VERTEX_SHADER, {kVertex}, "vertex");
    bool ok = vs && link(g_linProg, vs, {kPrecision, kLinearize}, "linearize") &&
              link(g_crtProg, vs, {kPrecision, kCrtHead, g_kind == Kind::Lottes ? kLottes : kLottesFast}, "crt") &&
              link(g_presentProg, vs, {kPrecision, kPresent}, "present");
    if (vs) gl.DeleteShader(vs);
    gl.UseProgram((GLuintT)prevProg);
    if (!ok) return false;
    g_failed = false;
    g_ready = true;
    g_watchStart = 0;
    log("crt: shaders ready");
    return true;
}

// The screen fell behind: draw the CRT picture smaller.
void watchRate(int srcH, int fullH) {
    const Uint32 now = SDL_GetTicks();
    if (!g_watchStart) { g_watchStart = now; g_watchFrames = 0; return; }
    ++g_watchFrames;
    if (now - g_watchStart < 2000) return;
    const double fps = g_watchFrames * 1000.0 / (now - g_watchStart);
    g_watchStart = now;
    g_watchFrames = 0;
    if (++g_watchRounds <= 1) return;  // the first two seconds include loading
    if (fps >= g_refresh * 0.92) return;
    // Floor: three screen lines per game line, or the full size if that's less.
    const double floorScale = std::min(1.0, 3.0 * srcH / (double)fullH);
    const double scale = std::sqrt(std::min(1.0, g_budget / g_lastFull));
    if (scale <= floorScale + 1e-3) return;
    g_budget *= 0.7;
    log("crt: %.1f fps shown (screen %.0f Hz): drawing smaller, %.0f%% of full size", fps, g_refresh,
        100.0 * std::max(floorScale, std::sqrt(std::min(1.0, g_budget / g_lastFull))));
}

}  // namespace

Kind kindNamed(const char* s) {
    if (!s) return Kind::Off;
    if (std::strcmp(s, "lottes") == 0) return Kind::Lottes;
    if (std::strcmp(s, "lottes-fast") == 0) return Kind::LottesFast;
    return Kind::Off;
}

void setKind(Kind k) { g_kind = k; }

void setRefresh(double hz) { if (hz > 1.0) g_refresh = hz; }

bool wanted() { return g_kind != Kind::Off && !g_failed; }

void store(const void* data, unsigned w, unsigned h, size_t pitch, int format) {
    if (!wanted() || !data || w == 0 || h == 0) return;
    const bool wide = format == 1;
    const size_t row = (size_t)w * (wide ? 4 : 2);
    g_frame.resize(row * h);
    const uint8_t* in = (const uint8_t*)data;
    for (unsigned y = 0; y < h; ++y) {
        uint8_t* out = &g_frame[(size_t)y * row];
        const uint8_t* src = in + (size_t)y * pitch;
        if (format == 0) {  // 0RGB1555 -> RGB565
            const uint16_t* s16 = (const uint16_t*)src;
            uint16_t* d16 = (uint16_t*)out;
            for (unsigned x = 0; x < w; ++x) {
                const uint16_t v = s16[x];
                const uint16_t g5 = (v >> 5) & 31;
                d16[x] = (uint16_t)(((v >> 10) & 31) << 11 | (g5 << 1 | g5 >> 4) << 5 | (v & 31));
            }
        } else {
            std::memcpy(out, src, row);
        }
    }
    g_w = w;
    g_h = h;
    g_wide = wide;
    g_dirty = true;
}

bool draw(const SDL_Rect& srcIn, const SDL_Rect& dst, int angle, int winW, int winH) {
    if (!wanted() || g_w == 0 || srcIn.w <= 0 || srcIn.h <= 0 || dst.w <= 0 || dst.h <= 0) return false;
    if (!g_ready && !init()) return false;
    SDL_Rect src = srcIn;
    src.w = std::min(src.w, (int)g_w - src.x);
    src.h = std::min(src.h, (int)g_h - src.y);
    if (src.w <= 0 || src.h <= 0) return false;

    // Everything SDL 2.0.7's GLES2 renderer caches or assumes, put back below.
    GLintT prog = 0, abuf = 0, tex = 0, active = 0, unpack = 1, fbo = 0, view[4] = {};
    GLintT en0 = 0, en1 = 0;
    gl.GetIntegerv(kGL_CURRENT_PROGRAM, &prog);
    gl.GetIntegerv(kGL_ARRAY_BUFFER_BINDING, &abuf);
    gl.GetIntegerv(kGL_FRAMEBUFFER_BINDING, &fbo);
    gl.GetIntegerv(kGL_ACTIVE_TEXTURE, &active);
    gl.ActiveTexture(kGL_TEXTURE0);
    gl.GetIntegerv(kGL_TEXTURE_BINDING_2D, &tex);
    gl.GetIntegerv(kGL_UNPACK_ALIGNMENT, &unpack);
    gl.GetIntegerv(kGL_VIEWPORT, view);
    gl.GetVertexAttribiv(0, kGL_VERTEX_ATTRIB_ARRAY_ENABLED, &en0);
    gl.GetVertexAttribiv(1, kGL_VERTEX_ATTRIB_ARRAY_ENABLED, &en1);
    const bool blend = gl.IsEnabled(kGL_BLEND), scissor = gl.IsEnabled(kGL_SCISSOR_TEST);

    gl.BindBuffer(kGL_ARRAY_BUFFER, 0);  // our arrays are client-side
    if (blend) gl.Disable(kGL_BLEND);
    if (scissor) gl.Disable(kGL_SCISSOR_TEST);
    gl.EnableVertexAttribArray(0);
    gl.EnableVertexAttribArray(1);
    bool ok = true;

    // The frame, uploaded.
    if (!g_frameTex) gl.GenTextures(1, &g_frameTex);
    gl.BindTexture(kGL_TEXTURE_2D, g_frameTex);
    const bool fresh = g_dirty;
    if (g_dirty) {
        gl.PixelStorei(kGL_UNPACK_ALIGNMENT, 1);
        const unsigned fmt = g_wide ? kGL_RGBA : kGL_RGB, type = g_wide ? kGL_UNSIGNED_BYTE : kGL_UNSIGNED_SHORT_5_6_5;
        if (g_w != g_texW || g_h != g_texH || g_wide != g_texWide) {
            gl.TexImage2D(kGL_TEXTURE_2D, 0, (int)fmt, (int)g_w, (int)g_h, 0, fmt, type, g_frame.data());
            gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_MIN_FILTER, kGL_NEAREST);
            gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_MAG_FILTER, kGL_NEAREST);
            gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_WRAP_S, kGL_CLAMP_TO_EDGE);
            gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_WRAP_T, kGL_CLAMP_TO_EDGE);
            g_texW = g_w; g_texH = g_h; g_texWide = g_wide;
            log("crt: frame texture %ux%u %s", g_w, g_h, g_wide ? "XRGB8888" : "RGB565");
        } else {
            gl.TexSubImage2D(kGL_TEXTURE_2D, 0, 0, 0, (int)g_w, (int)g_h, fmt, type, g_frame.data());
        }
        g_dirty = false;
    }

    // 1. Linear light, at the game's size.
    const bool sameSrc = g_linSrc.x == src.x && g_linSrc.y == src.y && g_linSrc.w == src.w && g_linSrc.h == src.h;
    if (fresh || !sameSrc) {
        ok = resize(g_lin, src.w, src.h, kGL_NEAREST, "linear frame");
        if (ok) {
            gl.BindFramebuffer(kGL_FRAMEBUFFER, g_lin.fbo);
            gl.BindTexture(kGL_TEXTURE_2D, g_frameTex);
            gl.Viewport(0, 0, src.w, src.h);
            gl.UseProgram(g_linProg.id);
            gl.Uniform2f(g_linProg.uTexSize, (float)g_texW, (float)g_texH);
            gl.Uniform2f(g_linProg.uSrcOrigin, (float)src.x, (float)src.y);
            gl.Uniform2f(g_linProg.uSrcSize, (float)src.w, (float)src.h);
            gl.Uniform1f(g_linProg.uSwap, g_texWide ? 1.0f : 0.0f);
            quad(kFullPos, kFullUV);
            g_linSrc = src;
        }
    }

    // 2. The CRT picture: offscreen at the budget, or straight to the screen.
    const double full = (double)dst.w * dst.h;
    g_lastFull = full;
    const double floorScale = std::min(1.0, 3.0 * src.h / (double)dst.h);
    const double scale = std::max(floorScale, std::sqrt(std::min(1.0, g_budget / full)));
    const bool offscreen = scale < 0.999;
    const int ow = offscreen ? std::max(1, (int)std::lround(dst.w * scale)) : dst.w;
    const int oh = offscreen ? std::max(1, (int)std::lround(dst.h * scale)) : dst.h;

    // dst's corners turned clockwise around its center (screen y runs down),
    // then to clip space. Strip order: top-left, top-right, bottom-left,
    // bottom-right of the upright picture.
    const float cx = dst.x + dst.w * 0.5f, cy = dst.y + dst.h * 0.5f;
    const int a = ((angle % 360) + 360) % 360;
    const float cs = a == 90 ? 0.0f : a == 180 ? -1.0f : a == 270 ? 0.0f : 1.0f;
    const float sn = a == 90 ? 1.0f : a == 180 ? 0.0f : a == 270 ? -1.0f : 0.0f;
    const float hx[4] = {-dst.w * 0.5f, dst.w * 0.5f, -dst.w * 0.5f, dst.w * 0.5f};
    const float hy[4] = {-dst.h * 0.5f, -dst.h * 0.5f, dst.h * 0.5f, dst.h * 0.5f};
    float screenPos[8];
    const float screenUV[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    for (int i = 0; i < 4; ++i) {
        const float x = cx + hx[i] * cs - hy[i] * sn, y = cy + hx[i] * sn + hy[i] * cs;
        screenPos[i * 2] = 2.0f * x / winW - 1.0f;
        screenPos[i * 2 + 1] = 1.0f - 2.0f * y / winH;
    }

    if (ok && offscreen) ok = resize(g_out, ow, oh, kGL_LINEAR, "crt picture");
    if (ok) {
        gl.BindTexture(kGL_TEXTURE_2D, g_lin.tex);
        gl.UseProgram(g_crtProg.id);
        gl.Uniform2f(g_crtProg.uSrcSize, (float)src.w, (float)src.h);
        gl.Uniform2f(g_crtProg.uOutSize, (float)ow, (float)oh);
        if (offscreen) {
            gl.BindFramebuffer(kGL_FRAMEBUFFER, g_out.fbo);
            gl.Viewport(0, 0, ow, oh);
            quad(kFullPos, kFullUV);
            // 3. Onto the screen.
            gl.BindFramebuffer(kGL_FRAMEBUFFER, (GLuintT)fbo);
            gl.Viewport(0, 0, winW, winH);
            gl.BindTexture(kGL_TEXTURE_2D, g_out.tex);
            gl.UseProgram(g_presentProg.id);
            quad(screenPos, screenUV);
        } else {
            gl.BindFramebuffer(kGL_FRAMEBUFFER, (GLuintT)fbo);
            gl.Viewport(0, 0, winW, winH);
            quad(screenPos, screenUV);
        }
    }
    gl.BindFramebuffer(kGL_FRAMEBUFFER, (GLuintT)fbo);

    if (!en0) gl.DisableVertexAttribArray(0);
    if (!en1) gl.DisableVertexAttribArray(1);
    gl.Viewport(view[0], view[1], view[2], view[3]);
    if (scissor) gl.Enable(kGL_SCISSOR_TEST);
    if (blend) gl.Enable(kGL_BLEND);
    gl.BindBuffer(kGL_ARRAY_BUFFER, (GLuintT)abuf);
    gl.UseProgram((GLuintT)prog);
    gl.BindTexture(kGL_TEXTURE_2D, (GLuintT)tex);
    gl.ActiveTexture((unsigned)active);
    gl.PixelStorei(kGL_UNPACK_ALIGNMENT, unpack);
    if (!g_errorLogged) {
        if (unsigned e = gl.GetError()) { log("crt: GL error 0x%x while drawing", e); g_errorLogged = true; }
    }
    if (!ok) {
        log("crt: an offscreen picture failed, filter off");
        g_failed = true;
        return false;
    }
    watchRate(src.h, dst.h);
    return true;
}

void shutdown() {
    if (!g_ready) return;
    if (g_frameTex) gl.DeleteTextures(1, &g_frameTex);
    for (Target* t : {&g_lin, &g_out}) {
        if (t->tex) gl.DeleteTextures(1, &t->tex);
        if (t->fbo) gl.DeleteFramebuffers(1, &t->fbo);
        *t = Target{};
    }
    for (Program* p : {&g_linProg, &g_crtProg, &g_presentProg})
        if (p->id) { gl.DeleteProgram(p->id); p->id = 0; }
    g_frameTex = 0;
    g_ready = false;
}

}  // namespace CrtFilter
