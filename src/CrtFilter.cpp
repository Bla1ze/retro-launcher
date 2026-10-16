#include "CrtFilter.h"

#include "Library.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

using Library::log;

namespace CrtFilter {
namespace {

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

// Shared by both filters. uSrcOrigin/uSrcSize: the shown part of the frame
// (blank edge columns cropped), in frame pixels. uOutSize: the picture's size
// on screen, upright. texel() takes a position in source pixels.
const char* kHead = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D uTex;
uniform vec2 uTexSize;
uniform vec2 uSrcOrigin;
uniform vec2 uSrcSize;
uniform vec2 uOutSize;
uniform float uSwap;
varying vec2 vUV;
vec3 texel(vec2 px) {
    vec3 c = texture2D(uTex, (px + uSrcOrigin) / uTexSize).rgb;
    return uSwap > 0.5 ? c.bgr : c;
}
)";

// crt-lottes, the original:
//   PUBLIC DOMAIN CRT STYLED SCAN-LINE SHADER
//     by Timothy Lottes
//   This is more along the style of a really good CGA arcade monitor.
//   With RGB inputs instead of NTSC.
//   Please take and use, change, or whatever.
// From libretro's glsl-shaders (crt/shaders/crt-lottes.glsl) with its default
// parameters, the stretched VGA shadow mask (3) and bloom on; reads the frame
// through texel() and blacks out samples beyond its edges.
const char* kLottes = R"(
const float hardScan = -8.0;
const float hardPix = -3.0;
const float warpX = 0.031;
const float warpY = 0.041;
const float maskDark = 0.5;
const float maskLight = 1.5;
const float brightBoost = 1.0;
const float hardBloomPix = -1.5;
const float hardBloomScan = -2.0;
const float bloomAmount = 0.15;
const float shape = 2.0;

float ToLinear1(float c) { return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
vec3 ToLinear(vec3 c) { return vec3(ToLinear1(c.r), ToLinear1(c.g), ToLinear1(c.b)); }
float ToSrgb1(float c) { return (c < 0.0031308 ? c * 12.92 : 1.055 * pow(c, 0.41666) - 0.055); }
vec3 ToSrgb(vec3 c) { return vec3(ToSrgb1(c.r), ToSrgb1(c.g), ToSrgb1(c.b)); }

// Nearest emulated sample given floating point position and texel offset.
vec3 Fetch(vec2 pos, vec2 off) {
    vec2 p = floor(pos * uSrcSize + off) + vec2(0.5, 0.5);
    if (p.x < 0.0 || p.y < 0.0 || p.x > uSrcSize.x || p.y > uSrcSize.y) return vec3(0.0);
    return ToLinear(brightBoost * texel(p));
}
// Distance in emulated pixels to nearest texel.
vec2 Dist(vec2 pos) {
    pos = pos * uSrcSize;
    return -((pos - floor(pos)) - vec2(0.5));
}
// 1D Gaussian.
float Gaus(float pos, float scale) { return exp2(scale * pow(abs(pos), shape)); }
// 3-tap Gaussian filter along horz line.
vec3 Horz3(vec2 pos, float off) {
    vec3 b = Fetch(pos, vec2(-1.0, off));
    vec3 c = Fetch(pos, vec2( 0.0, off));
    vec3 d = Fetch(pos, vec2( 1.0, off));
    float dst = Dist(pos).x;
    float scale = hardPix;
    float wb = Gaus(dst - 1.0, scale);
    float wc = Gaus(dst + 0.0, scale);
    float wd = Gaus(dst + 1.0, scale);
    return (b * wb + c * wc + d * wd) / (wb + wc + wd);
}
// 5-tap Gaussian filter along horz line.
vec3 Horz5(vec2 pos, float off) {
    vec3 a = Fetch(pos, vec2(-2.0, off));
    vec3 b = Fetch(pos, vec2(-1.0, off));
    vec3 c = Fetch(pos, vec2( 0.0, off));
    vec3 d = Fetch(pos, vec2( 1.0, off));
    vec3 e = Fetch(pos, vec2( 2.0, off));
    float dst = Dist(pos).x;
    float scale = hardPix;
    float wa = Gaus(dst - 2.0, scale);
    float wb = Gaus(dst - 1.0, scale);
    float wc = Gaus(dst + 0.0, scale);
    float wd = Gaus(dst + 1.0, scale);
    float we = Gaus(dst + 2.0, scale);
    return (a * wa + b * wb + c * wc + d * wd + e * we) / (wa + wb + wc + wd + we);
}
// 7-tap Gaussian filter along horz line.
vec3 Horz7(vec2 pos, float off) {
    vec3 a = Fetch(pos, vec2(-3.0, off));
    vec3 b = Fetch(pos, vec2(-2.0, off));
    vec3 c = Fetch(pos, vec2(-1.0, off));
    vec3 d = Fetch(pos, vec2( 0.0, off));
    vec3 e = Fetch(pos, vec2( 1.0, off));
    vec3 f = Fetch(pos, vec2( 2.0, off));
    vec3 g = Fetch(pos, vec2( 3.0, off));
    float dst = Dist(pos).x;
    float scale = hardBloomPix;
    float wa = Gaus(dst - 3.0, scale);
    float wb = Gaus(dst - 2.0, scale);
    float wc = Gaus(dst - 1.0, scale);
    float wd = Gaus(dst + 0.0, scale);
    float we = Gaus(dst + 1.0, scale);
    float wf = Gaus(dst + 2.0, scale);
    float wg = Gaus(dst + 3.0, scale);
    return (a * wa + b * wb + c * wc + d * wd + e * we + f * wf + g * wg) / (wa + wb + wc + wd + we + wf + wg);
}
// Return scanline weight.
float Scan(vec2 pos, float off) { return Gaus(Dist(pos).y + off, hardScan); }
// Return scanline weight for bloom.
float BloomScan(vec2 pos, float off) { return Gaus(Dist(pos).y + off, hardBloomScan); }
// Allow nearest three lines to effect pixel.
vec3 Tri(vec2 pos) {
    vec3 a = Horz3(pos, -1.0);
    vec3 b = Horz5(pos, 0.0);
    vec3 c = Horz3(pos, 1.0);
    return a * Scan(pos, -1.0) + b * Scan(pos, 0.0) + c * Scan(pos, 1.0);
}
// Small bloom.
vec3 Bloom(vec2 pos) {
    vec3 a = Horz5(pos, -2.0);
    vec3 b = Horz7(pos, -1.0);
    vec3 c = Horz7(pos, 0.0);
    vec3 d = Horz7(pos, 1.0);
    vec3 e = Horz5(pos, 2.0);
    return a * BloomScan(pos, -2.0) + b * BloomScan(pos, -1.0) + c * BloomScan(pos, 0.0) +
           d * BloomScan(pos, 1.0) + e * BloomScan(pos, 2.0);
}
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
    vec3 outColor = Tri(pos);
    outColor += Bloom(pos) * bloomAmount;
    outColor *= Mask(gl_FragCoord.xy * 1.000001);
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
// default parameters: the 8-tap filter, warp, tone mapping, mask type 1.
// (Its contrast and saturation steps are no-ops at those defaults.)
const char* kLottesFast = R"(
const float CRT_GAMMA = 2.4;
const float SCANLINE_THINNESS = 0.5;
const float SCAN_BLUR = 2.5;
const float MASK_INTENSITY = 0.54;
const float CURVATURE = 0.02;
const float CORNER = 3.0;

float FromSrgb1(float c) { return (c <= 0.04045) ? c * (1.0 / 12.92) : pow(c * (1.0 / 1.055) + (0.055 / 1.055), CRT_GAMMA); }
vec3 FromSrgb(vec3 c) { return vec3(FromSrgb1(c.r), FromSrgb1(c.g), FromSrgb1(c.b)); }
float ToSrgb1(float c) { return (c < 0.0031308 ? c * 12.92 : 1.055 * pow(c, 0.41666) - 0.055); }
vec3 ToSrgb(vec3 c) { return vec3(ToSrgb1(c.r), ToSrgb1(c.g), ToSrgb1(c.b)); }

// uv: 0-1 over the shown source, at texel centers.
vec3 CrtsFetch(vec2 uv) { return FromSrgb(texel(clamp(uv * uSrcSize, vec2(0.5), uSrcSize - vec2(0.5)))); }

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
    kGL_TEXTURE_MIN_FILTER = 0x2801, kGL_TEXTURE_MAG_FILTER = 0x2800, kGL_NEAREST = 0x2600,
    kGL_TEXTURE_WRAP_S = 0x2802, kGL_TEXTURE_WRAP_T = 0x2803, kGL_CLAMP_TO_EDGE = 0x812F,
    kGL_TRIANGLE_STRIP = 0x0005, kGL_FLOAT = 0x1406, kGL_SHADING_LANGUAGE_VERSION = 0x8B8C
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
           load(gl.GetString, "glGetString") & load(gl.GetError, "glGetError");
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

// ------------------------------------------------------------------ state

Kind g_kind = Kind::Off;
bool g_ready = false, g_failed = false;
GLuintT g_prog = 0, g_tex = 0;
GLintT g_uTex = -1, g_uTexSize = -1, g_uSrcOrigin = -1, g_uSrcSize = -1, g_uOutSize = -1, g_uSwap = -1;
// The latest frame, packed rows: 4 bytes a pixel (B, G, R, X) or RGB565.
std::vector<uint8_t> g_frame;
unsigned g_w = 0, g_h = 0;
bool g_wide = true;  // 32-bit
bool g_dirty = false;
unsigned g_texW = 0, g_texH = 0;
bool g_texWide = true;
bool g_errorLogged = false;

bool init() {
    g_failed = true;
    if (!SDL_GL_GetCurrentContext()) { log("crt: no GL context (software renderer), filter off"); return false; }
    if (!loadFns()) return false;
    const unsigned char* ver = gl.GetString(kGL_SHADING_LANGUAGE_VERSION);
    log("crt: %s, GLSL %s", g_kind == Kind::Lottes ? "Lottes" : "Lottes (fast)", ver ? (const char*)ver : "?");
    GLuintT vs = compile(kGL_VERTEX_SHADER, {kVertex}, "vertex");
    GLuintT fs = compile(kGL_FRAGMENT_SHADER, {kHead, g_kind == Kind::Lottes ? kLottes : kLottesFast}, "fragment");
    if (!vs || !fs) {
        if (vs) gl.DeleteShader(vs);
        if (fs) gl.DeleteShader(fs);
        return false;
    }
    GLintT prevProg = 0;
    gl.GetIntegerv(kGL_CURRENT_PROGRAM, &prevProg);
    g_prog = gl.CreateProgram();
    gl.AttachShader(g_prog, vs);
    gl.AttachShader(g_prog, fs);
    gl.BindAttribLocation(g_prog, 0, "aPos");
    gl.BindAttribLocation(g_prog, 1, "aUV");
    gl.LinkProgram(g_prog);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    GLintT ok = 0;
    gl.GetProgramiv(g_prog, kGL_LINK_STATUS, &ok);
    if (!ok) {
        char msg[1024] = {};
        gl.GetProgramInfoLog(g_prog, sizeof msg, nullptr, msg);
        log("crt: shader didn't link: %s", msg);
        gl.DeleteProgram(g_prog);
        g_prog = 0;
        return false;
    }
    g_uTex = gl.GetUniformLocation(g_prog, "uTex");
    g_uTexSize = gl.GetUniformLocation(g_prog, "uTexSize");
    g_uSrcOrigin = gl.GetUniformLocation(g_prog, "uSrcOrigin");
    g_uSrcSize = gl.GetUniformLocation(g_prog, "uSrcSize");
    g_uOutSize = gl.GetUniformLocation(g_prog, "uOutSize");
    g_uSwap = gl.GetUniformLocation(g_prog, "uSwap");
    gl.UseProgram(g_prog);
    gl.Uniform1i(g_uTex, 0);
    gl.UseProgram((GLuintT)prevProg);
    g_failed = false;
    g_ready = true;
    log("crt: shader ready");
    return true;
}

}  // namespace

Kind kindNamed(const char* s) {
    if (!s) return Kind::Off;
    if (std::strcmp(s, "lottes") == 0) return Kind::Lottes;
    if (std::strcmp(s, "lottes-fast") == 0) return Kind::LottesFast;
    return Kind::Off;
}

void setKind(Kind k) { g_kind = k; }

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

bool draw(const SDL_Rect& src, const SDL_Rect& dst, int angle, int winW, int winH) {
    if (!wanted() || g_w == 0 || src.w <= 0 || src.h <= 0 || dst.w <= 0 || dst.h <= 0) return false;
    if (!g_ready && !init()) return false;

    // Everything SDL 2.0.7's GLES2 renderer caches or assumes, put back below.
    GLintT prog = 0, abuf = 0, tex = 0, active = 0, unpack = 1, view[4] = {};
    GLintT en0 = 0, en1 = 0;
    gl.GetIntegerv(kGL_CURRENT_PROGRAM, &prog);
    gl.GetIntegerv(kGL_ARRAY_BUFFER_BINDING, &abuf);
    gl.GetIntegerv(kGL_ACTIVE_TEXTURE, &active);
    gl.ActiveTexture(kGL_TEXTURE0);
    gl.GetIntegerv(kGL_TEXTURE_BINDING_2D, &tex);
    gl.GetIntegerv(kGL_UNPACK_ALIGNMENT, &unpack);
    gl.GetIntegerv(kGL_VIEWPORT, view);
    gl.GetVertexAttribiv(0, kGL_VERTEX_ATTRIB_ARRAY_ENABLED, &en0);
    gl.GetVertexAttribiv(1, kGL_VERTEX_ATTRIB_ARRAY_ENABLED, &en1);
    const bool blend = gl.IsEnabled(kGL_BLEND), scissor = gl.IsEnabled(kGL_SCISSOR_TEST);

    if (!g_tex) gl.GenTextures(1, &g_tex);
    gl.BindTexture(kGL_TEXTURE_2D, g_tex);
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

    // dst's corners turned clockwise around its center (screen y runs down),
    // then to clip space. Strip order: top-left, top-right, bottom-left,
    // bottom-right of the upright picture.
    const float cx = dst.x + dst.w * 0.5f, cy = dst.y + dst.h * 0.5f;
    const int a = ((angle % 360) + 360) % 360;
    const float cs = a == 90 ? 0.0f : a == 180 ? -1.0f : a == 270 ? 0.0f : 1.0f;
    const float sn = a == 90 ? 1.0f : a == 180 ? 0.0f : a == 270 ? -1.0f : 0.0f;
    const float hx[4] = {-dst.w * 0.5f, dst.w * 0.5f, -dst.w * 0.5f, dst.w * 0.5f};
    const float hy[4] = {-dst.h * 0.5f, -dst.h * 0.5f, dst.h * 0.5f, dst.h * 0.5f};
    float pos[8], uv[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    for (int i = 0; i < 4; ++i) {
        const float x = cx + hx[i] * cs - hy[i] * sn, y = cy + hx[i] * sn + hy[i] * cs;
        pos[i * 2] = 2.0f * x / winW - 1.0f;
        pos[i * 2 + 1] = 1.0f - 2.0f * y / winH;
    }

    gl.UseProgram(g_prog);
    gl.Uniform2f(g_uTexSize, (float)g_texW, (float)g_texH);
    gl.Uniform2f(g_uSrcOrigin, (float)src.x, (float)src.y);
    gl.Uniform2f(g_uSrcSize, (float)std::min<int>(src.w, (int)g_texW - src.x), (float)std::min<int>(src.h, (int)g_texH - src.y));
    gl.Uniform2f(g_uOutSize, (float)dst.w, (float)dst.h);
    gl.Uniform1f(g_uSwap, g_texWide ? 1.0f : 0.0f);  // XRGB8888 rows hold B, G, R, X
    gl.BindBuffer(kGL_ARRAY_BUFFER, 0);              // our arrays are client-side
    if (blend) gl.Disable(kGL_BLEND);
    if (scissor) gl.Disable(kGL_SCISSOR_TEST);
    gl.Viewport(0, 0, winW, winH);
    gl.EnableVertexAttribArray(0);
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(0, 2, kGL_FLOAT, 0, 0, pos);
    gl.VertexAttribPointer(1, 2, kGL_FLOAT, 0, 0, uv);
    gl.DrawArrays(kGL_TRIANGLE_STRIP, 0, 4);

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
    return true;
}

void shutdown() {
    if (!g_ready) return;
    if (g_tex) gl.DeleteTextures(1, &g_tex);
    if (g_prog) gl.DeleteProgram(g_prog);
    g_tex = g_prog = 0;
    g_ready = false;
}

}  // namespace CrtFilter
