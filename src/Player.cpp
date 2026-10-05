#include "Player.h"
#include "Arcade.h"

#include "Ambient.h"
#include "AppFont.h"
#include "Gfx.h"
#include "Theme.h"
#include "GamePanels.h"
#include "DisplayProfile.h"
#include "Library.h"
#include "libretro_min.h"
#include "vendor/stb_image.h"  // implementation lives in GamePanels.cpp

#include <SDL.h>

#include <dlfcn.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// A minimal libretro front-end: one game per process, then back to the menu by
// exec. Proven on a 4KP with Genesis Plus GX on the backglass (byog-player-test).

namespace {

using Library::log;

// ------------------------------------------------------------ small helpers

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool hasExt(const std::string& name, const std::string& ext) {
    std::string n = lower(name);
    return n.size() > ext.size() + 1 && n[n.size() - ext.size() - 1] == '.' &&
           n.compare(n.size() - ext.size(), ext.size(), ext) == 0;
}

std::string baseName(const std::string& p) {
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string stem(const std::string& name) {
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    bool ok = n <= 0 || std::fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    std::fclose(f);
    return ok;
}

bool writeFile(const std::string& path, const void* data, size_t size) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data, 1, size, f) == size;
    std::fclose(f);
    return ok;
}

std::vector<std::string> splitExts(const char* list) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = list ? list : ""; ; ++p) {
        if (*p == '|' || *p == 0) { if (!cur.empty()) out.push_back(lower(cur)); cur.clear(); if (!*p) break; }
        else cur += *p;
    }
    return out;
}

// ------------------------------------------------------- zip via system libz
// The cabinet ships libz.so.1; the SDK image has no arm64 zlib headers, so we
// declare z_stream ourselves and dlopen it.

struct ZStream {
    const uint8_t* next_in; unsigned avail_in; unsigned long total_in;
    uint8_t* next_out; unsigned avail_out; unsigned long total_out;
    const char* msg; void* state;
    void* zalloc; void* zfree; void* opaque;
    int data_type; unsigned long adler; unsigned long reserved;
};

bool inflateRaw(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen) {
    static void* z = ::dlopen("libz.so.1", RTLD_NOW);
    if (!z) { log("dlopen libz.so.1 failed: %s", ::dlerror()); return false; }
    auto init = (int (*)(ZStream*, int, const char*, int))::dlsym(z, "inflateInit2_");
    auto inflate = (int (*)(ZStream*, int))::dlsym(z, "inflate");
    auto end = (int (*)(ZStream*))::dlsym(z, "inflateEnd");
    if (!init || !inflate || !end) { log("libz symbols missing"); return false; }
    ZStream s;
    std::memset(&s, 0, sizeof(s));
    s.next_in = in; s.avail_in = (unsigned)inLen;
    s.next_out = out; s.avail_out = (unsigned)outLen;
    if (init(&s, -15, "1.2.11", (int)sizeof(ZStream)) != 0) return false;
    int r = inflate(&s, 4 /* Z_FINISH */);
    end(&s);
    return r == 1 /* Z_STREAM_END */;
}

uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// Extracts the entry whose extension the core accepts (else the first file).
bool unzipRom(const std::vector<uint8_t>& zip, const std::vector<std::string>& exts, std::string& name,
              std::vector<uint8_t>& out) {
    if (zip.size() < 22) return false;
    size_t eocd = std::string::npos;
    for (size_t i = zip.size() - 22;; --i) {
        if (rd32(&zip[i]) == 0x06054b50) { eocd = i; break; }
        if (i == 0 || zip.size() - i > 65557) break;
    }
    if (eocd == std::string::npos) return false;
    unsigned entries = rd16(&zip[eocd + 10]);
    size_t cd = rd32(&zip[eocd + 16]);
    struct Entry { std::string name; unsigned method; size_t csize, usize, local; };
    std::vector<Entry> list;
    for (unsigned i = 0; i < entries && cd + 46 <= zip.size(); ++i) {
        if (rd32(&zip[cd]) != 0x02014b50) break;
        Entry e;
        e.method = rd16(&zip[cd + 10]);
        e.csize = rd32(&zip[cd + 20]);
        e.usize = rd32(&zip[cd + 24]);
        unsigned nl = rd16(&zip[cd + 28]), xl = rd16(&zip[cd + 30]), cl = rd16(&zip[cd + 32]);
        e.local = rd32(&zip[cd + 42]);
        e.name.assign((const char*)&zip[cd + 46], nl);
        if (!e.name.empty() && e.name.back() != '/') list.push_back(e);
        cd += 46 + nl + xl + cl;
    }
    if (list.empty()) return false;
    const Entry* pick = &list[0];
    for (const Entry& e : list) {
        bool match = false;
        for (const std::string& x : exts) match = match || (x != "zip" && hasExt(e.name, x));
        if (match) { pick = &e; break; }
    }
    log("zip: %zu file(s), using '%s' (method %u, %zu -> %zu bytes)", list.size(), pick->name.c_str(),
        pick->method, pick->csize, pick->usize);
    size_t lh = pick->local;
    if (lh + 30 > zip.size() || rd32(&zip[lh]) != 0x04034b50) return false;
    size_t data = lh + 30 + rd16(&zip[lh + 26]) + rd16(&zip[lh + 28]);
    if (data + pick->csize > zip.size()) return false;
    out.resize(pick->usize);
    name = baseName(pick->name);
    if (pick->method == 0) { std::memcpy(out.data(), &zip[data], pick->usize); return true; }
    if (pick->method == 8) return inflateRaw(&zip[data], pick->csize, out.data(), out.size());
    log("zip: unsupported compression method %u", pick->method);
    return false;
}

// ------------------------------------------------------------------- core

struct Core {
    void* handle = nullptr;
    void (*init)() = nullptr;
    void (*deinit)() = nullptr;
    unsigned (*api_version)() = nullptr;
    void (*get_system_info)(retro_system_info*) = nullptr;
    void (*get_system_av_info)(retro_system_av_info*) = nullptr;
    void (*set_environment)(retro_environment_t) = nullptr;
    void (*set_video_refresh)(retro_video_refresh_t) = nullptr;
    void (*set_audio_sample)(retro_audio_sample_t) = nullptr;
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t) = nullptr;
    void (*set_input_poll)(retro_input_poll_t) = nullptr;
    void (*set_input_state)(retro_input_state_t) = nullptr;
    void (*set_controller_port_device)(unsigned, unsigned) = nullptr;
    void (*run)() = nullptr;
    bool (*load_game)(const retro_game_info*) = nullptr;
    void (*unload_game)() = nullptr;
    void* (*get_memory_data)(unsigned) = nullptr;
    size_t (*get_memory_size)(unsigned) = nullptr;
    // Optional (save states / reset); null when a core lacks them.
    size_t (*serialize_size)() = nullptr;
    bool (*serialize)(void*, size_t) = nullptr;
    bool (*unserialize)(const void*, size_t) = nullptr;
    void (*reset)() = nullptr;
};

template <typename T>
bool bindSym(void* h, T& fn, const char* name) {
    fn = reinterpret_cast<T>(::dlsym(h, name));
    if (!fn) log("core is missing %s", name);
    return fn != nullptr;
}

std::string loadCore(const std::string& path, Core& c) {
    c.handle = ::dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!c.handle) {
        const char* e = ::dlerror();
        log("dlopen %s: %s", path.c_str(), e ? e : "?");
        return "The emulator core would not load.";
    }
    bool ok = bindSym(c.handle, c.init, "retro_init") & bindSym(c.handle, c.deinit, "retro_deinit") &
              bindSym(c.handle, c.api_version, "retro_api_version") &
              bindSym(c.handle, c.get_system_info, "retro_get_system_info") &
              bindSym(c.handle, c.get_system_av_info, "retro_get_system_av_info") &
              bindSym(c.handle, c.set_environment, "retro_set_environment") &
              bindSym(c.handle, c.set_video_refresh, "retro_set_video_refresh") &
              bindSym(c.handle, c.set_audio_sample, "retro_set_audio_sample") &
              bindSym(c.handle, c.set_audio_sample_batch, "retro_set_audio_sample_batch") &
              bindSym(c.handle, c.set_input_poll, "retro_set_input_poll") &
              bindSym(c.handle, c.set_input_state, "retro_set_input_state") &
              bindSym(c.handle, c.set_controller_port_device, "retro_set_controller_port_device") &
              bindSym(c.handle, c.run, "retro_run") & bindSym(c.handle, c.load_game, "retro_load_game") &
              bindSym(c.handle, c.unload_game, "retro_unload_game") &
              bindSym(c.handle, c.get_memory_data, "retro_get_memory_data") &
              bindSym(c.handle, c.get_memory_size, "retro_get_memory_size");
    c.serialize_size = reinterpret_cast<size_t (*)()>(::dlsym(c.handle, "retro_serialize_size"));
    c.serialize = reinterpret_cast<bool (*)(void*, size_t)>(::dlsym(c.handle, "retro_serialize"));
    c.unserialize = reinterpret_cast<bool (*)(const void*, size_t)>(::dlsym(c.handle, "retro_unserialize"));
    c.reset = reinterpret_cast<void (*)()>(::dlsym(c.handle, "retro_reset"));
    return ok ? "" : "The emulator core is not a libretro core.";
}

// ------------------------------------------------------------- shared state

std::string g_systemDir, g_saveDir;
retro_pixel_format g_pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
retro_system_av_info g_av{};
unsigned g_coreRotation = 0;  // RETRO_ENVIRONMENT_SET_ROTATION: picture turned 90 * n degrees counter-clockwise

SDL_Renderer* g_renderer = nullptr;
SDL_Texture* g_texture = nullptr;
Uint32 g_textureFormat = 0;
int g_texW = 0, g_texH = 0, g_frameW = 0, g_frameH = 0;
bool g_newFrame = false;
unsigned long g_frames = 0, g_dupes = 0;

Ambient g_ambient;
bool g_sharp = false;  // Settings > Scaling: Sharp / Pixel-perfect

// CRT scanlines: two rows per game line (clear, dark), stretched over the
// picture with smoothing so the lines stay soft at any size. 64 pixels wide and
// made once per height: a 1-pixel-wide strip is the kind of odd texture some GPU
// drivers mishandle (v0.6.0 froze with scanlines on).
SDL_Texture* makeScanlines(SDL_Renderer* r, int lines, int darkness) {
    const int w = 64;
    std::vector<uint32_t> px((size_t)w * lines * 2);
    for (int y = 0; y < lines * 2; ++y) {
        uint32_t v = (y & 1) ? ((uint32_t)darkness << 24) : 0u;
        std::fill(px.begin() + (size_t)y * w, px.begin() + (size_t)(y + 1) * w, v);
    }
    SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, lines * 2);
    if (t) {
        SDL_UpdateTexture(t, nullptr, px.data(), w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    }
    log("scanlines texture %dx%d: %s", w, lines * 2, t ? "ok" : SDL_GetError());
    return t;
}
std::vector<int16_t> g_audioBatch;
std::vector<SDL_GameController*> g_pads;
uint16_t g_buttons = 0;
int16_t g_analog[2][2] = {};  // [left/right stick][x/y], for cores that read analog (PSP, Dreamcast)

// Watchdog: if a frame takes longer than this, a core has hung; go back to
// the menu instead of leaving the cabinet frozen. execv is async-signal-safe.
std::vector<std::string> g_watchdogArgs;
std::vector<char*> g_watchdogArgv;
std::string g_watchdogPath;

std::vector<std::string> g_crashArgs;
std::vector<char*> g_crashArgv;

int g_logFd = -1;  // launcher.log, so signal handlers can leave a note there too

void onWatchdog(int) {
    const char msg[] = "[launcher] watchdog: the game stopped responding, returning to the menu\n";
    ssize_t ignored = ::write(1, msg, sizeof(msg) - 1);
    if (g_logFd >= 0) ignored = ::write(g_logFd, msg + 11, sizeof(msg) - 12);
    (void)ignored;
    ::execv(g_watchdogPath.c_str(), g_watchdogArgv.data());
    ::_exit(1);
}

// A core that crashes (segfault, abort from an uncaught C++ exception...) would
// otherwise take the whole app down and drop the cabinet back to its own menu.
void onCrash(int sig) {
    char msg[96];
    int n = std::snprintf(msg, sizeof(msg), "[launcher] the game crashed (signal %d), returning to the menu\n", sig);
    ssize_t ignored = ::write(1, msg, n > 0 ? (size_t)n : 0);
    if (g_logFd >= 0 && n > 11) ignored = ::write(g_logFd, msg + 11, (size_t)n - 11);
    (void)ignored;
    ::execv(g_watchdogPath.c_str(), g_crashArgv.data());
    ::_exit(1);
}

// SDL's default assertion handler prompts on stdin and then exits the process
// outright (no signal, so the crash guard can't step in) - v0.6.1 dropped back to
// the firmware menu that way. Log it once and carry on instead.
SDL_AssertState onSdlAssert(const SDL_AssertData* d, void*) {
    if (d && d->trigger_count == 0)
        Library::log("SDL assertion '%s' at %s:%d (%s) - ignored", d->condition, d->filename, d->linenum, d->function);
    return SDL_ASSERTION_IGNORE;
}

void installHandlers() {
    SDL_SetAssertionHandler(onSdlAssert, nullptr);
    // SA_NODEFER: these handlers exec, and a signal blocked while its handler
    // runs would stay blocked in the menu and every later game.
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_NODEFER | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = onCrash;
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) ::sigaction(sig, &sa, nullptr);
    sa.sa_flags = SA_NODEFER;
    sa.sa_handler = onWatchdog;
    ::sigaction(SIGALRM, &sa, nullptr);
    sigset_t set;
    sigemptyset(&set);
    for (int sig : {SIGALRM, SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaddset(&set, sig);
    ::sigprocmask(SIG_UNBLOCK, &set, nullptr);
}

// Core options. Cores declare them with SET_VARIABLES ("Description; default|other|...");
// we answer GET_VARIABLE with the default, or with a value from
// data/core-options.cfg (key = value). Some cores abort if a variable is missing.
std::map<std::string, std::string> g_options;
std::map<std::string, std::string> g_optionOverrides;
// What the core declared, in its order, for the Core options menu.
struct OptDef { std::string key, desc; std::vector<std::string> values; };
std::vector<OptDef> g_optDefs;
bool g_optionsDirty = false;  // answered once by GET_VARIABLE_UPDATE
std::string g_optionsPath;

// RetroArch's core options format: key = "value". Unquoted values are accepted.
void saveOptionOverrides() {
    FILE* f = std::fopen(g_optionsPath.c_str(), "w");
    if (!f) { log("could not save %s: %s", g_optionsPath.c_str(), std::strerror(errno)); return; }
    for (auto& kv : g_optionOverrides) std::fprintf(f, "%s = \"%s\"\n", kv.first.c_str(), kv.second.c_str());
    std::fclose(f);
}

void setOption(const std::string& key, const std::string& value) {
    g_options[key] = value;
    g_optionOverrides[key] = value;
    g_optionsDirty = true;
    saveOptionOverrides();
    log("core option %s = %s", key.c_str(), value.c_str());
}

void loadOptionOverrides(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        auto trim = [](std::string v) {
            size_t a = v.find_first_not_of(" \t\r"), b = v.find_last_not_of(" \t\r");
            return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
        };
        std::string v = trim(line.substr(eq + 1));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        g_optionOverrides[trim(line.substr(0, eq))] = v;
    }
}

// Options the launcher decides itself, kept at the core's default and left out
// of the Core options menu: the picture's orientation is handled here (vertical
// games turned to fit the screen they play on), and the cores' "vertical" /
// TATE modes, meant for monitors turned on their side, would undo it.
bool pinnedOption(const std::string& key) {
    return key == "fbneo-vertical-mode" || key == "mame2003-plus_tate_mode" || key == "reicast_screen_rotation";
}

void declareOption(const char* key, const char* spec) {
    std::string v = spec ? spec : "";
    size_t semi = v.find("; ");
    std::string opts = semi == std::string::npos ? v : v.substr(semi + 2);
    std::string def = opts.substr(0, opts.find('|'));
    if (pinnedOption(key)) { g_options[key] = def; return; }
    auto o = g_optionOverrides.find(key);
    g_options[key] = o != g_optionOverrides.end() ? o->second : def;
    OptDef d;
    d.key = key;
    d.desc = semi == std::string::npos ? key : v.substr(0, semi);
    for (size_t a = 0;;) {
        size_t b = opts.find('|', a);
        d.values.push_back(opts.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    for (OptDef& e : g_optDefs)
        if (e.key == d.key) { e = d; return; }
    g_optDefs.push_back(d);
}

// --------------------------------------------------------------- callbacks

void coreLog(enum retro_log_level level, const char* fmt, ...) {
    // Debug output is skipped: MAME writes a line for nearly everything (21 MB
    // in one session), all of it to the USB stick while the game runs.
    if (level == RETRO_LOG_DEBUG) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t n = std::strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    static const char* names[] = {"debug", "info", "warn", "error"};
    log("core %s: %s", names[(unsigned)level <= 3 ? level : 1], buf);
}

// ------------------------------------------------------------- GPU cores
// Cores that draw with OpenGL ES (Flycast) get a context of their own, so they
// can't disturb the state SDL 2.0.7's GLES2 renderer caches, and a framebuffer
// to draw into. Each finished frame is read back into the normal picture path
// (rotation, bezels, scanlines, pause menu, glow all unchanged).
//
// SDL's renderer remembers its context in a static of its own and only
// re-makes it current when that changes, so after every call into the core the
// renderer's context is made current again here (CoreGL).

typedef unsigned int GLuintT;
struct GlFns {
    void (*GenFramebuffers)(int, GLuintT*);
    void (*BindFramebuffer)(unsigned, GLuintT);
    void (*DeleteFramebuffers)(int, const GLuintT*);
    void (*GenTextures)(int, GLuintT*);
    void (*BindTexture)(unsigned, GLuintT);
    void (*DeleteTextures)(int, const GLuintT*);
    void (*TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
    void (*TexParameteri)(unsigned, unsigned, int);
    void (*FramebufferTexture2D)(unsigned, unsigned, unsigned, GLuintT, int);
    void (*GenRenderbuffers)(int, GLuintT*);
    void (*BindRenderbuffer)(unsigned, GLuintT);
    void (*DeleteRenderbuffers)(int, const GLuintT*);
    void (*RenderbufferStorage)(unsigned, unsigned, int, int);
    void (*FramebufferRenderbuffer)(unsigned, unsigned, unsigned, GLuintT);
    unsigned (*CheckFramebufferStatus)(unsigned);
    void (*ReadPixels)(int, int, int, int, unsigned, unsigned, void*);
    void (*PixelStorei)(unsigned, int);
    const unsigned char* (*GetString)(unsigned);
} gl{};
enum : unsigned {
    kGL_FRAMEBUFFER = 0x8D40, kGL_RENDERBUFFER = 0x8D41, kGL_COLOR_ATTACHMENT0 = 0x8CE0,
    kGL_DEPTH_STENCIL_ATTACHMENT = 0x821A, kGL_DEPTH24_STENCIL8 = 0x88F0, kGL_FRAMEBUFFER_COMPLETE = 0x8CD5,
    kGL_TEXTURE_2D = 0x0DE1, kGL_RGBA = 0x1908, kGL_UNSIGNED_BYTE = 0x1401, kGL_TEXTURE_MIN_FILTER = 0x2801,
    kGL_TEXTURE_MAG_FILTER = 0x2800, kGL_NEAREST = 0x2600, kGL_PACK_ALIGNMENT = 0x0D05, kGL_VERSION = 0x1F02,
    kGL_RENDERER = 0x1F01
};

SDL_Window* g_window = nullptr;
SDL_GLContext g_rendererCtx = nullptr;  // SDL's renderer
SDL_GLContext g_coreCtx = nullptr;      // the core's, when it renders with the GPU
retro_hw_render_callback g_hw{};
GLuintT g_fbo = 0, g_fboTex = 0, g_fboDepth = 0;
int g_fboW = 0, g_fboH = 0;
std::vector<uint8_t> g_hwRaw, g_hwFrame;  // read back (RGBA, GL rows) / converted (XRGB8888, top first)
unsigned g_hwW = 0, g_hwH = 0;
bool g_hwPending = false;
double g_readbackMs = 0.0;
unsigned long g_readbacks = 0;

// While alive, the core's GL context is current (no-op for software cores).
struct CoreGL {
    CoreGL() { if (g_coreCtx) SDL_GL_MakeCurrent(g_window, g_coreCtx); }
    ~CoreGL() { if (g_coreCtx) SDL_GL_MakeCurrent(g_window, g_rendererCtx); }
};

uintptr_t hwCurrentFramebuffer() { return g_fbo; }
retro_proc_address_t hwProcAddress(const char* sym) { return (retro_proc_address_t)SDL_GL_GetProcAddress(sym); }

template <typename F> bool loadGl(F& fn, const char* name) {
    fn = (F)SDL_GL_GetProcAddress(name);
    if (!fn) log("gl: %s missing", name);
    return fn != nullptr;
}

// Framebuffer the core draws into (core context current).
bool makeFramebuffer(int w, int h) {
    if (g_fbo) { gl.DeleteFramebuffers(1, &g_fbo); gl.DeleteTextures(1, &g_fboTex); gl.DeleteRenderbuffers(1, &g_fboDepth); }
    gl.GenTextures(1, &g_fboTex);
    gl.BindTexture(kGL_TEXTURE_2D, g_fboTex);
    gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_MIN_FILTER, kGL_NEAREST);
    gl.TexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_MAG_FILTER, kGL_NEAREST);
    gl.TexImage2D(kGL_TEXTURE_2D, 0, kGL_RGBA, w, h, 0, kGL_RGBA, kGL_UNSIGNED_BYTE, nullptr);
    gl.BindTexture(kGL_TEXTURE_2D, 0);
    gl.GenRenderbuffers(1, &g_fboDepth);
    gl.BindRenderbuffer(kGL_RENDERBUFFER, g_fboDepth);
    gl.RenderbufferStorage(kGL_RENDERBUFFER, kGL_DEPTH24_STENCIL8, w, h);
    gl.BindRenderbuffer(kGL_RENDERBUFFER, 0);
    gl.GenFramebuffers(1, &g_fbo);
    gl.BindFramebuffer(kGL_FRAMEBUFFER, g_fbo);
    gl.FramebufferTexture2D(kGL_FRAMEBUFFER, kGL_COLOR_ATTACHMENT0, kGL_TEXTURE_2D, g_fboTex, 0);
    gl.FramebufferRenderbuffer(kGL_FRAMEBUFFER, kGL_DEPTH_STENCIL_ATTACHMENT, kGL_RENDERBUFFER, g_fboDepth);
    unsigned status = gl.CheckFramebufferStatus(kGL_FRAMEBUFFER);
    gl.BindFramebuffer(kGL_FRAMEBUFFER, 0);
    g_fboW = w; g_fboH = h;
    log("gpu: framebuffer %dx%d: %s (0x%x)", w, h, status == kGL_FRAMEBUFFER_COMPLETE ? "complete" : "INCOMPLETE", status);
    return status == kGL_FRAMEBUFFER_COMPLETE;
}

// SET_HW_RENDER (inside load_game): a GLES context for the core, left current
// for the rest of load_game.
bool startHwRender(retro_hw_render_callback* cb) {
    if (cb->context_type != RETRO_HW_CONTEXT_OPENGLES2 && cb->context_type != RETRO_HW_CONTEXT_OPENGLES3 &&
        cb->context_type != RETRO_HW_CONTEXT_OPENGLES_VERSION) {
        log("gpu: core asked for context type %d (only OpenGL ES here)", (int)cb->context_type);
        return false;
    }
    if (!g_window || !g_rendererCtx) { log("gpu: no GL renderer to share the window with"); return false; }
    if (!g_coreCtx) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
        for (int major : {3, 2}) {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
            g_coreCtx = SDL_GL_CreateContext(g_window);  // and makes it current
            if (g_coreCtx) break;
            log("gpu: GLES %d.0 context failed: %s", major, SDL_GetError());
        }
        if (!g_coreCtx) { SDL_GL_MakeCurrent(g_window, g_rendererCtx); return false; }
        bool ok = loadGl(gl.GenFramebuffers, "glGenFramebuffers") & loadGl(gl.BindFramebuffer, "glBindFramebuffer") &
                  loadGl(gl.DeleteFramebuffers, "glDeleteFramebuffers") & loadGl(gl.GenTextures, "glGenTextures") &
                  loadGl(gl.BindTexture, "glBindTexture") & loadGl(gl.DeleteTextures, "glDeleteTextures") &
                  loadGl(gl.TexImage2D, "glTexImage2D") & loadGl(gl.TexParameteri, "glTexParameteri") &
                  loadGl(gl.FramebufferTexture2D, "glFramebufferTexture2D") &
                  loadGl(gl.GenRenderbuffers, "glGenRenderbuffers") & loadGl(gl.BindRenderbuffer, "glBindRenderbuffer") &
                  loadGl(gl.DeleteRenderbuffers, "glDeleteRenderbuffers") &
                  loadGl(gl.RenderbufferStorage, "glRenderbufferStorage") &
                  loadGl(gl.FramebufferRenderbuffer, "glFramebufferRenderbuffer") &
                  loadGl(gl.CheckFramebufferStatus, "glCheckFramebufferStatus") &
                  loadGl(gl.ReadPixels, "glReadPixels") & loadGl(gl.PixelStorei, "glPixelStorei") &
                  loadGl(gl.GetString, "glGetString");
        if (!ok) {
            SDL_GL_MakeCurrent(g_window, g_rendererCtx);
            SDL_GL_DeleteContext(g_coreCtx);
            g_coreCtx = nullptr;
            return false;
        }
        const unsigned char* ver = gl.GetString(kGL_VERSION);
        const unsigned char* rend = gl.GetString(kGL_RENDERER);
        log("gpu: core context %s | %s", ver ? (const char*)ver : "?", rend ? (const char*)rend : "?");
    }
    cb->get_current_framebuffer = hwCurrentFramebuffer;
    cb->get_proc_address = hwProcAddress;
    g_hw = *cb;
    log("gpu: core wants type %d, version %u.%u, depth %d, stencil %d, bottom-left origin %d", (int)cb->context_type,
        cb->version_major, cb->version_minor, cb->depth, cb->stencil, cb->bottom_left_origin);
    return true;
}

// RETRO_HW_FRAME_BUFFER_VALID (core context current): read the frame back.
// The upload to SDL waits until the renderer's context is current again.
void readHwFrame(unsigned w, unsigned h) {
    if (!g_fbo || w == 0 || h == 0) return;
    w = std::min<unsigned>(w, (unsigned)g_fboW);
    h = std::min<unsigned>(h, (unsigned)g_fboH);
    Uint64 t0 = SDL_GetPerformanceCounter();
    g_hwRaw.resize((size_t)w * h * 4);
    gl.BindFramebuffer(kGL_FRAMEBUFFER, g_fbo);
    gl.PixelStorei(kGL_PACK_ALIGNMENT, 4);
    gl.ReadPixels(0, 0, (int)w, (int)h, kGL_RGBA, kGL_UNSIGNED_BYTE, g_hwRaw.data());
    // RGBA bytes -> XRGB8888 (B,G,R,A in memory); GL's first row is the bottom.
    g_hwFrame.resize(g_hwRaw.size());
    for (unsigned y = 0; y < h; ++y) {
        const uint8_t* src = &g_hwRaw[(size_t)(g_hw.bottom_left_origin ? h - 1 - y : y) * w * 4];
        uint8_t* dst = &g_hwFrame[(size_t)y * w * 4];
        for (unsigned x = 0; x < w; ++x, src += 4, dst += 4) {
            dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = 0xff;
        }
    }
    g_hwW = w; g_hwH = h;
    g_hwPending = true;
    g_readbackMs += (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
    ++g_readbacks;
}

void videoRefresh(const void* data, unsigned w, unsigned h, size_t pitch);

// After a core call, with the renderer's context current again.
void flushHwFrame() {
    if (!g_hwPending) return;
    g_hwPending = false;
    g_pixelFormat = RETRO_PIXEL_FORMAT_XRGB8888;
    videoRefresh(g_hwFrame.data(), g_hwW, g_hwH, (size_t)g_hwW * 4);
}

bool environment(unsigned cmd, void* data) {
    switch (cmd & ~RETRO_ENVIRONMENT_EXPERIMENTAL) {
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool*)data = true; return true;
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER: *(unsigned*)data = RETRO_HW_CONTEXT_OPENGLES3; return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER: return startHwRender((retro_hw_render_callback*)data);
    case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT: return true;
    case RETRO_ENVIRONMENT_SET_ROTATION:
        g_coreRotation = *(const unsigned*)data & 3;
        log("core asks for rotation %u (x90 counter-clockwise)", g_coreRotation);
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE:
        if (data) log("core message: %s", ((const retro_message*)data)->msg);
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char**)data = g_systemDir.c_str(); return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char**)data = g_saveDir.c_str(); return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        retro_pixel_format f = *(const retro_pixel_format*)data;
        if ((unsigned)f > RETRO_PIXEL_FORMAT_RGB565) return false;
        g_pixelFormat = f;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
        for (const retro_variable* v = (const retro_variable*)data; v && v->key; ++v) declareOption(v->key, v->value);
        log("core declared %zu option(s)", g_options.size());
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        retro_variable* v = (retro_variable*)data;
        auto it = v && v->key ? g_options.find(v->key) : g_options.end();
        if (it == g_options.end()) {
            auto o = v && v->key ? g_optionOverrides.find(v->key) : g_optionOverrides.end();
            if (o == g_optionOverrides.end()) {
                log("core asked for undeclared option %s", v && v->key ? v->key : "?");
                if (v) v->value = nullptr;
                return false;
            }
            it = g_options.insert({o->first, o->second}).first;
        }
        v->value = it->second.c_str();
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool*)data = g_optionsDirty;  // tells the core to re-read its options
        g_optionsDirty = false;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((retro_log_callback*)data)->log = coreLog; return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        g_av = *(const retro_system_av_info*)data;
        // A GPU core's picture grew past its framebuffer (e.g. a higher internal
        // resolution): make a bigger one. Called from inside the core, so its
        // context is current.
        if (g_coreCtx && g_fbo && ((int)g_av.geometry.max_width > g_fboW || (int)g_av.geometry.max_height > g_fboH))
            makeFramebuffer(std::max<int>(g_fboW, g_av.geometry.max_width), std::max<int>(g_fboH, g_av.geometry.max_height));
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY: g_av.geometry = *(const retro_game_geometry*)data; return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE: *(unsigned*)data = 0; return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned*)data = 0; return true;
    default: {
        // Untested cores: log each unhandled request once, so one cabinet run
        // shows what a core wanted.
        static std::vector<unsigned> seen;
        if (std::find(seen.begin(), seen.end(), cmd) == seen.end()) {
            seen.push_back(cmd);
            log("environment request %u (0x%x) not handled", cmd & 0xffff, cmd);
        }
        return false;
    }
    }
}

Uint32 sdlFormat(retro_pixel_format f) {
    if (f == RETRO_PIXEL_FORMAT_XRGB8888) return SDL_PIXELFORMAT_ARGB8888;
    if (f == RETRO_PIXEL_FORMAT_RGB565) return SDL_PIXELFORMAT_RGB565;
    return SDL_PIXELFORMAT_RGB555;
}

// Blank edge columns. Master System games (and some NES games) blank the
// leftmost 8 pixels, so the picture carries a black strip on one side and looks
// off-center. Every half second, count fully black columns at each edge (up to
// 16); a result seen three times in a row becomes the crop.
int g_cropL = 0, g_cropR = 0;
bool g_noCrop = false;   // arcade: black edges are part of the game
int g_candL = -1, g_candR = -1, g_candSeen = 0;
unsigned g_cropW = 0;

bool nearBlack(const uint8_t* row, unsigned x, int fmt) {
    if (fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
        uint32_t v;
        std::memcpy(&v, row + x * 4, 4);
        return ((v >> 16) & 0xff) < 16 && ((v >> 8) & 0xff) < 16 && (v & 0xff) < 16;
    }
    uint16_t v;
    std::memcpy(&v, row + x * 2, 2);
    if (fmt == RETRO_PIXEL_FORMAT_RGB565) return ((v >> 11) & 31) < 2 && ((v >> 5) & 63) < 4 && (v & 31) < 2;
    return ((v >> 10) & 31) < 2 && ((v >> 5) & 31) < 2 && (v & 31) < 2;
}

void detectBlankEdges(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (w != g_cropW) { g_cropW = w; g_cropL = g_cropR = 0; g_candSeen = 0; }
    if (g_frames % 30 != 0 || w < 64 || h < 16) return;
    const uint8_t* base = static_cast<const uint8_t*>(data);
    auto blankCol = [&](unsigned x) {
        for (unsigned y = 0; y < h; y += 4)
            if (!nearBlack(base + y * pitch, x, (int)g_pixelFormat)) return false;
        return true;
    };
    int l = 0, r = 0;
    while (l < 16 && blankCol((unsigned)l)) ++l;
    while (r < 16 && blankCol(w - 1 - (unsigned)r)) ++r;
    if (blankCol(w / 2) || l == 16 || r == 16) return;  // a dark scene, not a border: decide later
    if (l < 4) l = 0;
    if (r < 4) r = 0;
    if (l == g_candL && r == g_candR) ++g_candSeen;
    else { g_candL = l; g_candR = r; g_candSeen = 1; }
    if (g_candSeen >= 3 && (l != g_cropL || r != g_cropR)) {
        g_cropL = l;
        g_cropR = r;
        log("cropping blank edges: %d px left, %d px right", l, r);
    }
}

// Bezel artwork (The Bezel Project format): a 1920x1080 PNG with a transparent
// window for the game. Per game media/<system>/bezels/<rom>.png, else the system
// one media/<system>/bezel.png. The window is found from the alpha channel.
struct Bezel {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
    SDL_Rect window{0, 0, 0, 0};  // in image pixels
};

Bezel loadBezel(const std::string& appDir, const std::string& sys, const std::string& romFile) {
    Bezel b;
    std::string stemName = romFile.substr(0, romFile.find_last_of('.'));
    for (const std::string& path : {appDir + "/media/" + sys + "/bezels/" + stemName + ".png",
                                    appDir + "/media/" + sys + "/bezel.png"}) {
        int n = 0;
        uint8_t* px = stbi_load(path.c_str(), &b.w, &b.h, &n, 4);
        if (!px) continue;
        // Bounding box of the clearly transparent pixels near the middle rows.
        int x0 = b.w, x1 = -1, y0 = b.h, y1 = -1;
        for (int y = 0; y < b.h; y += 2)
            for (int x = 0; x < b.w; x += 2)
                if (px[((size_t)y * b.w + x) * 4 + 3] < 40) {
                    x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
                }
        if (x1 > x0 && y1 > y0 && (x1 - x0) * (y1 - y0) > b.w * b.h / 5)
            b.window = {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
        else
            b.window = {0, 0, b.w, b.h};
        b.tex = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, b.w, b.h);
        if (b.tex) {
            SDL_UpdateTexture(b.tex, nullptr, px, b.w * 4);
            SDL_SetTextureBlendMode(b.tex, SDL_BLENDMODE_BLEND);
        }
        stbi_image_free(px);
        log("bezel %s (%dx%d, window %d,%d %dx%d)", path.c_str(), b.w, b.h, b.window.x, b.window.y, b.window.w,
            b.window.h);
        return b;
    }
    return b;
}

void videoRefresh(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (data == RETRO_HW_FRAME_BUFFER_VALID) { readHwFrame(w, h); return; }
    if (!data) { ++g_dupes; return; }
    Uint32 fmt = sdlFormat(g_pixelFormat);
    int tw = (int)std::max(w, g_av.geometry.max_width), th = (int)std::max(h, g_av.geometry.max_height);
    if (!g_texture || fmt != g_textureFormat || tw > g_texW || th > g_texH) {
        if (g_texture) SDL_DestroyTexture(g_texture);
        // Filtering is fixed when a texture is created (the cabinet's SDL may
        // predate SDL_SetTextureScaleMode), so pick it via the hint and put
        // "linear" back for everything else (fonts, shapes).
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, g_sharp ? "nearest" : "linear");
        g_texture = SDL_CreateTexture(g_renderer, fmt, SDL_TEXTUREACCESS_STREAMING, tw, th);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        g_textureFormat = fmt; g_texW = tw; g_texH = th;
        log("texture %dx%d %s: %s", tw, th, SDL_GetPixelFormatName(fmt), g_texture ? "ok" : SDL_GetError());
    }
    if (!g_texture) return;
    SDL_Rect r{0, 0, (int)w, (int)h};
    SDL_UpdateTexture(g_texture, &r, data, (int)pitch);
    g_frameW = (int)w; g_frameH = (int)h;
    g_ambient.sample(data, w, h, pitch, (int)g_pixelFormat);
    if (!g_noCrop) detectBlankEdges(data, w, h, pitch);
    g_newFrame = true;
    ++g_frames;
}

size_t audioBatch(const int16_t* data, size_t frames) {
    g_audioBatch.insert(g_audioBatch.end(), data, data + frames * 2);
    return frames;
}

void audioSample(int16_t l, int16_t r) { g_audioBatch.push_back(l); g_audioBatch.push_back(r); }
void inputPoll() {}

int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port == 0 && (device & 0xff) == RETRO_DEVICE_ANALOG && index <= 1 && id <= 1) return g_analog[index][id];
    if (port != 0 || (device & 0xff) != RETRO_DEVICE_JOYPAD) return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)g_buttons;
    return id < 16 ? (int16_t)((g_buttons >> id) & 1) : 0;
}

// Dynamic rate control: the screen's real refresh is never exactly what the
// mode claims, so a vsync-locked stream slowly over- or under-fills. Resample
// each frame's audio by a tiny ratio (at most +/-0.5%, inaudible) steered by
// how far the queue is from its target, so it hovers at the target instead.
std::vector<int16_t> g_drcOut;
double g_drcPos = 0.0;  // fractional read position carried across frames

int16_t g_drcPrev[2] = {0, 0};
bool g_drcHavePrev = false;

const std::vector<int16_t>& resampleStereo(const std::vector<int16_t>& in, double ratio) {
    // Prepend the previous batch's last frame so interpolation runs straight
    // across batch boundaries; g_drcPos is measured from that frame.
    static std::vector<int16_t> buf;
    buf.clear();
    if (g_drcHavePrev) { buf.push_back(g_drcPrev[0]); buf.push_back(g_drcPrev[1]); }
    buf.insert(buf.end(), in.begin(), in.end());
    g_drcOut.clear();
    size_t frames = buf.size() / 2;
    if (frames < 2) { g_drcOut = in; return g_drcOut; }
    double step = 1.0 / ratio;  // input frames advanced per output frame
    double pos = g_drcHavePrev ? g_drcPos : 0.0;
    while (pos < frames - 1) {
        size_t i = (size_t)pos;
        double t = pos - i;
        for (int c = 0; c < 2; ++c) {
            double v = buf[i * 2 + c] * (1.0 - t) + buf[(i + 1) * 2 + c] * t;
            g_drcOut.push_back((int16_t)std::max(-32768.0, std::min(32767.0, v)));
        }
        pos += step;
    }
    g_drcPos = pos - (frames - 1);
    g_drcPrev[0] = buf[(frames - 1) * 2];
    g_drcPrev[1] = buf[(frames - 1) * 2 + 1];
    g_drcHavePrev = true;
    return g_drcOut;
}

// ------------------------------------------------------------------- input

void openPads() {
    for (SDL_GameController* p : g_pads) SDL_GameControllerClose(p);
    g_pads.clear();
    // Open EVERY controller: the cabinet splits its buttons across two devices.
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i))
            if (SDL_GameController* p = SDL_GameControllerOpen(i)) g_pads.push_back(p);
    log("%zu controller(s) open", g_pads.size());
}

bool padButton(SDL_GameControllerButton b) {
    for (SDL_GameController* p : g_pads)
        if (SDL_GameControllerGetButton(p, b)) return true;
    return false;
}

int padAxis(SDL_GameControllerAxis a) {
    int best = 0;
    for (SDL_GameController* p : g_pads) {
        int v = SDL_GameControllerGetAxis(p, a);
        if (std::abs(v) > std::abs(best)) best = v;
    }
    return best;
}

// What each cabinet button is doing right now.
bool cabDown(Library::Cab c) {
    const int dz = 16000;
    switch (c) {
    case Library::Cab::A: return padButton(SDL_CONTROLLER_BUTTON_A);
    case Library::Cab::B: return padButton(SDL_CONTROLLER_BUTTON_B);
    case Library::Cab::X: return padButton(SDL_CONTROLLER_BUTTON_X);
    case Library::Cab::Y: return padButton(SDL_CONTROLLER_BUTTON_Y);
    case Library::Cab::LB: return padButton(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    case Library::Cab::RB: return padButton(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    case Library::Cab::LB2: return padAxis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > dz;
    case Library::Cab::RB2: return padAxis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > dz;
    case Library::Cab::Start: return padButton(SDL_CONTROLLER_BUTTON_START);
    // SDL's Back never fires on the cabinet, but count it where it exists.
    case Library::Cab::Rewind: return padButton(SDL_CONTROLLER_BUTTON_LEFTSTICK) || padButton(SDL_CONTROLLER_BUTTON_BACK);
    case Library::Cab::Rewind2: return padButton(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    default: return false;
    }
}

// RetroPad buttons for a layout (the game's own, or the default for menus).
uint16_t readButtons(const Library::ButtonMap& map) {
    const int dz = 16000;
    uint16_t b = 0;
    auto set = [&b](int id, bool on) { if (on) b |= (uint16_t)(1u << id); };
    for (int id = 0; id < 16; ++id)
        if (map.src[id] != Library::Cab::None && cabDown(map.src[id])) set(id, true);
    int lx = padAxis(SDL_CONTROLLER_AXIS_LEFTX), ly = padAxis(SDL_CONTROLLER_AXIS_LEFTY);
    set(RETRO_DEVICE_ID_JOYPAD_UP, padButton(SDL_CONTROLLER_BUTTON_DPAD_UP) || ly < -dz);
    set(RETRO_DEVICE_ID_JOYPAD_DOWN, padButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ly > dz);
    set(RETRO_DEVICE_ID_JOYPAD_LEFT, padButton(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || lx < -dz);
    set(RETRO_DEVICE_ID_JOYPAD_RIGHT, padButton(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || lx > dz);
    return b;
}

// ------------------------------------------------------------- pause menu
// Drawn over the frozen game in the Neon style, in a 1280x720 logical space
// scaled to the screen (Gfx/AppFont/Theme bake per renderer).

// `winW` x `winH` is the area as the player sees it (portrait on the playfield,
// where the caller draws into a texture and turns it like the game picture).
void drawPauseMenu(SDL_Renderer* r, int winW, int winH, const std::string& title, const std::vector<std::string>& items,
                   const std::vector<bool>& enabled, int sel, const std::string& toast) {
    const float scale = std::min(winW, winH) / 720.0f;
    const float W = winW / scale, H = winH / scale;
    SDL_RenderSetScale(r, scale, scale);
    Gfx::rect(r, {0, 0, W, H}, {4, 6, 12, 190});
    const float rowH = 62.0f, gap = 10.0f, pw = 560.0f;
    const float ph = 150.0f + items.size() * (rowH + gap) + 40.0f;
    const float px = (W - pw) * 0.5f, py = (H - ph) * 0.5f;
    Gfx::softRect(r, {px, py, pw, ph}, 30.0f, 36.0f, {0, 0, 0, 200}, false);
    Gfx::panel(r, {px, py, pw, ph}, 30.0f, {30, 36, 58, 250}, {18, 22, 38, 250}, {255, 255, 255, 30}, 1.0f);
    Gfx::hGradient(r, {px + 30.0f, py, pw - 60.0f, 4.0f}, Theme::Accent, Theme::Accent2);
    Theme::glowTitle(r, title, W * 0.5f, py + 34.0f, 54.0f, Theme::Accent, AppFont::Face::Display);
    for (size_t i = 0; i < items.size(); ++i) {
        FRect row{px + 36.0f, py + 120.0f + i * (rowH + gap), pw - 72.0f, rowH};
        bool active = (int)i == sel;
        Theme::rowCard(r, row, active);
        SDL_Color c = !enabled[i] ? Theme::Faint : active ? Theme::Text : Theme::TextDim;
        AppFont::drawCentered(r, items[i], W * 0.5f, row.y + 15.0f, 30.0f, c);
    }
    AppFont::drawCentered(r, "A Select     B Resume", W * 0.5f, py + ph - 38.0f, 20.0f, Theme::Muted);
    if (!toast.empty()) {
        float tw = AppFont::measureWidth(r, toast, 24.0f) + 60.0f;
        FRect t{(W - tw) * 0.5f, py + ph + 20.0f, tw, 52.0f};
        Gfx::panel(r, t, 26.0f, {36, 42, 66, 250}, {24, 28, 46, 250}, Theme::alpha(Theme::Accent, 200), 2.0f);
        AppFont::drawCentered(r, toast, W * 0.5f, t.y + 12.0f, 24.0f, Theme::Text);
    }
    SDL_RenderSetScale(r, 1.0f, 1.0f);
}

void drawOptionsMenu(SDL_Renderer* r, int winW, int winH, const std::string& coreName, int sel, int top, int visible) {
    const float scale = std::min(winW, winH) / 720.0f;
    const float W = winW / scale, H = winH / scale;
    SDL_RenderSetScale(r, scale, scale);
    Gfx::rect(r, {0, 0, W, H}, {4, 6, 12, 200});
    const float rowH = 50.0f, gap = 6.0f;
    const float pw = std::min(W - 60.0f, 960.0f), ph = 150.0f + visible * (rowH + gap) + 70.0f;
    const float px = (W - pw) * 0.5f, py = (H - ph) * 0.5f;
    Gfx::softRect(r, {px, py, pw, ph}, 30.0f, 36.0f, {0, 0, 0, 200}, false);
    Gfx::panel(r, {px, py, pw, ph}, 30.0f, {30, 36, 58, 250}, {18, 22, 38, 250}, {255, 255, 255, 30}, 1.0f);
    Gfx::hGradient(r, {px + 30.0f, py, pw - 60.0f, 4.0f}, Theme::Accent, Theme::Accent2);
    Theme::glowTitle(r, "CORE OPTIONS", W * 0.5f, py + 26.0f, 46.0f, Theme::Accent, AppFont::Face::Display);
    AppFont::drawCentered(r, coreName, W * 0.5f, py + 84.0f, 20.0f, Theme::Muted);
    const float valueW = pw * 0.36f;
    for (int i = 0; i < visible && top + i < (int)g_optDefs.size(); ++i) {
        const OptDef& d = g_optDefs[top + i];
        FRect row{px + 28.0f, py + 124.0f + i * (rowH + gap), pw - 56.0f, rowH};
        bool active = top + i == sel;
        Theme::rowCard(r, row, active);
        std::string label = Theme::ellipsize(r, d.desc, row.w - valueW - 50.0f, 24.0f, AppFont::Face::Body);
        AppFont::draw(r, label, row.x + 18.0f, row.y + 12.0f, 24.0f, active ? Theme::Text : Theme::TextDim);
        auto it = g_options.find(d.key);
        std::string value = Theme::ellipsize(r, it == g_options.end() ? "?" : it->second, valueW - 40.0f, 24.0f,
                                             AppFont::Face::Body);
        float vw = AppFont::measureWidth(r, value, 24.0f);
        float vx = row.x + row.w - 22.0f - vw - (active ? 26.0f : 0.0f);
        AppFont::draw(r, value, vx, row.y + 12.0f, 24.0f, active ? Theme::accent() : Theme::Muted);
        if (active) {
            Gfx::triangle(r, {vx - 26.0f, row.y + rowH * 0.5f - 8.0f, 16.0f, 16.0f}, 180.0, Theme::accent());
            Gfx::triangle(r, {row.x + row.w - 38.0f, row.y + rowH * 0.5f - 8.0f, 16.0f, 16.0f}, 0.0, Theme::accent());
        }
    }
    char counter[32];
    std::snprintf(counter, sizeof(counter), "%d / %d", sel + 1, (int)g_optDefs.size());
    AppFont::drawCentered(r, std::string(counter) + "     LEFT/RIGHT Change     LB/RB Page     B Back", W * 0.5f,
                          py + ph - 56.0f, 19.0f, Theme::Muted);
    AppFont::drawCentered(r, "Some options take effect the next time the game starts", W * 0.5f, py + ph - 32.0f, 17.0f,
                          Theme::Faint);
    SDL_RenderSetScale(r, 1.0f, 1.0f);
}

bool fileExists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

} // namespace

int runPlayer(const std::string& appDir, const std::string& sysId, const std::string& romPath,
              const std::string& screenArg, int menuIndex, const std::string& returnTo, const std::string& coreArg) {
    const std::string menuSys = returnTo.empty() ? sysId : returnTo;  // where the menu comes back to
    Library::openLog(appDir, "play");
    auto fail = [&](const std::string& message) -> int {
        log("FAILED: %s", message.c_str());
        SDL_Quit();
        Library::execMenu(appDir, menuSys, menuIndex, message);
        return 1;
    };

    const Library::System* sys = Library::findSystem(sysId);
    if (!sys) return fail("Unknown system '" + sysId + "'.");
    Library::ScreenId screen = Library::ScreenId::Backglass;
    Library::parseScreen(screenArg, screen);
    Library::Settings settings;
    settings.load(appDir);
    log("system %s, rom %s, screen %s", sys->id.c_str(), romPath.c_str(), Library::screenName(screen));

    // Watchdog first, so even a hang while loading goes back to the menu.
    // signal() blocks SIGALRM while its handler runs, and the handler execs, so
    // that mask would be inherited by the menu and every later game: a second
    // hang would then never be caught. Unblock it explicitly every time.
    g_watchdogPath = Library::selfPath();
    g_watchdogArgs = {"retro-launcher", "--menu", appDir, menuSys, std::to_string(menuIndex),
                      "The game stopped responding."};
    for (std::string& a : g_watchdogArgs) g_watchdogArgv.push_back(&a[0]);
    g_watchdogArgv.push_back(nullptr);
    g_crashArgs = {"retro-launcher", "--menu", appDir, menuSys, std::to_string(menuIndex),
                   "The game crashed. This core may not support it."};
    for (std::string& a : g_crashArgs) g_crashArgv.push_back(&a[0]);
    g_crashArgv.push_back(nullptr);
    g_logFd = Library::logFd();
    installHandlers();
    ::alarm(30);
    g_optionsPath = appDir + "/data/core-options.cfg";
    loadOptionOverrides(g_optionsPath);

    // Screen: pick the connector for this model, before SDL_Init.
    DisplayProfile::Topology topo = DisplayProfile::detect();
    DisplayProfile::Screen target = screen == Library::ScreenId::Playfield ? topo.main
                                    : screen == Library::ScreenId::Dmd    ? topo.dmd
                                                                          : topo.backglass;
    if (!target.available) {
        log("%s not available on model '%s', using the backglass", Library::screenName(screen), topo.model.c_str());
        screen = Library::ScreenId::Backglass;
        target = topo.backglass.available ? topo.backglass : topo.main;
    }
    SDL_setenv("ForceConnectID", std::to_string(target.connectorId).c_str(), 1);
    if (topo.keepFirmwareDisplay && screen == Library::ScreenId::Playfield) SDL_setenv("SDL2_DISPLAY_PLANE_TYPE", "OVERLAY", 1);
    else ::unsetenv("SDL2_DISPLAY_PLANE_TYPE");
    // Backglass is upright (0, proven); playfield and DMD follow the panel's
    // mounting from DisplayProfile. All overridable as rotate.<screen>.
    int rotate = settings.rotation(screen, screen == Library::ScreenId::Backglass ? 0 : target.rotationDegrees);
    log("model %s: connector %u %dx%d, rotate %d", topo.model.c_str(), target.connectorId, target.width, target.height,
        rotate);

    // ROM: read, unzip if needed, and leave a real file for cores that want one.
    std::string where;
    std::string corePath = coreArg.empty() ? Library::findCore(appDir, *sys, where)
                                           : Library::findCoreFile(appDir, coreArg, where);
    if (corePath.empty() && !coreArg.empty()) corePath = Library::findCore(appDir, *sys, where);
    if (corePath.empty()) return fail("No emulator core found for " + sys->name + ".");
    log("core %s (%s)", corePath.c_str(), where.c_str());
    Core core;
    std::string err = loadCore(corePath, core);
    if (!err.empty()) return fail(err);
    retro_system_info info{};
    core.get_system_info(&info);
    log("core %s %s, extensions '%s', need_fullpath %d", info.library_name ? info.library_name : "?",
        info.library_version ? info.library_version : "?", info.valid_extensions ? info.valid_extensions : "",
        info.need_fullpath);

    // Arcade cores take the zip itself (and find parent / BIOS zips beside it).
    std::vector<std::string> coreExts = splitExts(info.valid_extensions);
    const bool zipToCore = hasExt(romPath, "zip") && std::find(coreExts.begin(), coreExts.end(), "zip") != coreExts.end();
    // Cores that open the file themselves get it where it is: a disc image can be
    // 700 MB, and a .cue needs its track files beside it.
    const bool direct = zipToCore || (info.need_fullpath && !hasExt(romPath, "zip"));
    g_noCrop = Library::isArcadeSystem(sys->id);
    std::vector<uint8_t> romData;
    if (!direct && !readFile(romPath, romData)) return fail("Could not read the ROM file.");
    std::string romName = baseName(romPath);
    // A game in a folder of its own ("Dolphin Blue/disc.gdi") is named after the
    // folder: for its title, art and bezel, and above all its saves, or every
    // disc.gdi game would share one set.
    // This game's button layout (its own, else its system's, else the default).
    std::string fileKey = romPath;
    {
        const std::string prefix = appDir + "/roms/" + sys->id + "/";
        if (fileKey.compare(0, prefix.size(), prefix) == 0) fileKey = fileKey.substr(prefix.size());
    }
    int mapScope = 0;
    const Library::ButtonMap buttonMap = Library::buttonMapFor(settings, sys->id, fileKey, &mapScope);
    log("buttons (%s): %s", mapScope == 2 ? "this game" : mapScope == 1 ? "system" : "default",
        Library::buttonMapToString(buttonMap).c_str());
    std::string gameFile = romName;
    {
        size_t slash = romPath.find_last_of('/');
        std::string parent = slash == std::string::npos ? "" : romPath.substr(0, slash);
        if (!parent.empty() && parent != appDir + "/roms/" + sys->id) {
            size_t dot = romName.find_last_of('.');
            gameFile = baseName(parent) + (dot == std::string::npos ? "" : romName.substr(dot));
        }
    }
    if (zipToCore) {
        if (!info.need_fullpath) return fail("This core wants the game in memory, not a zip.");
    } else if (hasExt(romPath, "zip")) {
        std::vector<std::string> exts = splitExts(info.valid_extensions);
        if (exts.empty()) exts = sys->extensions;
        std::vector<uint8_t> inner;
        std::string innerName;
        if (!unzipRom(romData, exts, innerName, inner)) return fail("Could not open the zip file.");
        romData.swap(inner);
        romName = innerName;
    }
    ::mkdir("/tmp/retrofe", 0755);
    std::string romFile = "/tmp/retrofe/" + romName;
    if (direct) {
        romFile = romPath;
    } else if (!writeFile(romFile, romData.data(), romData.size())) {
        log("could not write %s (%s); passing the original path", romFile.c_str(), std::strerror(errno));
        romFile = romPath;
    }

    g_systemDir = appDir + "/system";
    g_saveDir = appDir + "/saves/" + sys->id;

    // SDL on the chosen screen.
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0)
        return fail(std::string("Video would not start: ") + SDL_GetError());
    SDL_GameControllerAddMappingsFromFile("/userdata/customer_controller_db_3rd.txt");
    SDL_DisplayMode mode{};
    SDL_GetCurrentDisplayMode(0, &mode);
    SDL_Window* window = SDL_CreateWindow("Retro Launcher", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          mode.w > 0 ? mode.w : target.width, mode.h > 0 ? mode.h : target.height,
                                          SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!window) return fail(std::string("Could not open the screen: ") + SDL_GetError());
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    g_renderer = SDL_CreateRenderer(window, -1,
                                    SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
    if (!g_renderer) g_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    g_window = window;
    g_rendererCtx = SDL_GL_GetCurrentContext();  // the GLES2 renderer's (null for the software one)
    if (!g_renderer) return fail(std::string("Could not draw on the screen: ") + SDL_GetError());
    int winW = 0, winH = 0;
    SDL_GetRendererOutputSize(g_renderer, &winW, &winH);
    log("SDL %s, mode %dx%d @%dHz, output %dx%d", SDL_GetCurrentVideoDriver(), mode.w, mode.h, mode.refresh_rate,
        winW, winH);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    SDL_RenderPresent(g_renderer);
    Gfx::init(g_renderer, winH / 720.0f);  // pause menu shapes, baked at screen scale
    openPads();
    // Playfield + DMD while the game runs on the backglass: a "Now playing"
    // card with the controls. Drawn once on GamePanels' worker thread.
    std::unique_ptr<GamePanels> panels;
    if (settings.value("panels", "on") != "off") {
        panels.reset(new GamePanels());
        int up = panels->init(topo, GamePanels::Mode::Playing, target.connectorId);
        log("panels (playing): %d up", up);
        GamePanels::Item item;
        item.key = "play";
        item.system = sys->name;
        std::string file = gameFile, title = stem(file), tags;
        size_t cut = title.find_first_of("([");
        if (cut != std::string::npos && cut > 0) { tags = title.substr(cut); title = title.substr(0, cut); }
        while (!title.empty() && title.back() == ' ') title.pop_back();
        item.artPath = findArt(appDir, sys->id, file);
        Arcade::Entry ae;
        if (Library::isArcadeSystem(sys->id) && Arcade::lookup(appDir, baseName(corePath), lower(stem(file)), ae)) {
            title = ae.title;
            tags = ae.year + (ae.maker.empty() ? "" : "  " + ae.maker);
            if (item.artPath.empty()) item.artPath = findArt(appDir, sys->id, ae.title + ".zip");
        }
        if (Library::isArcadeSystem(sys->id) && item.artPath.empty())  // the other emulator's name for it
            for (const std::string& c : sys->cores)
                if (Arcade::lookup(appDir, c, lower(stem(file)), ae) && item.artPath.empty())
                    item.artPath = findArt(appDir, sys->id, ae.title + ".zip");
        item.title = title;
        item.detail = tags;
        item.controls = Library::controlHints(sys->id, buttonMap);
        item.consolePath = findConsoleArt(appDir, sys->id);
        panels->show(item, 0.0f);
    }
    Bezel bezel;
    // Settings > Picture sides: bezel (glow where a system has none), glow, black.
    // Older settings files used bezels=on|off and bars=ambient|black.
    std::string sides = settings.value("sides", "");
    if (sides.empty())
        sides = settings.value("bezels", "on") == "off" ? (settings.value("bars", "ambient") == "black" ? "black" : "glow")
                                                        : "bezel";
    if (sides == "bezel" && rotate == 0) bezel = loadBezel(appDir, sys->id, gameFile);
    std::string bars = sides == "black" ? "black" : "ambient";
    g_ambient.init(g_renderer, bars == "black" ? Ambient::Style::Black : Ambient::Style::Ambient);
    const std::string scaling = settings.value("scaling", "smooth");  // smooth | sharp | integer
    g_sharp = scaling == "sharp" || scaling == "integer";
    const std::string scan = settings.value("scanlines", "off");      // off | light | strong
    const int scanDark = scan == "strong" ? 150 : scan == "light" ? 80 : 0;
    SDL_Texture* scanTex = nullptr;
    int scanLines = 0;
    log("display: sides %s, scaling %s, scanlines %s", sides.c_str(), scaling.c_str(), scan.c_str());
    log("bars: %s", bars == "black" ? "black" : "ambient");

    // Start the game.
    core.set_environment(environment);
    core.init();
    core.set_video_refresh(videoRefresh);
    core.set_audio_sample(audioSample);
    core.set_audio_sample_batch(audioBatch);
    core.set_input_poll(inputPoll);
    core.set_input_state(inputState);
    retro_game_info game{romFile.c_str(), info.need_fullpath ? nullptr : romData.data(),
                         info.need_fullpath ? 0 : romData.size(), nullptr};
    bool loaded;
    { CoreGL glScope; loaded = core.load_game(&game); }  // SET_HW_RENDER may create the core's context in here
    if (!loaded) return fail("The emulator could not start this ROM.");
    core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    core.get_system_av_info(&g_av);
    if (g_coreCtx) {
        CoreGL glScope;
        int fw = std::max(640, (int)g_av.geometry.max_width), fh = std::max(480, (int)g_av.geometry.max_height);
        if (!makeFramebuffer(fw, fh)) return fail("The GPU could not set up this game's picture.");
        if (g_hw.context_reset) g_hw.context_reset();
        g_pixelFormat = RETRO_PIXEL_FORMAT_XRGB8888;
        g_noCrop = true;  // a GPU frame's black edges are the game's
    }
    log("loaded: %ux%u (max %ux%u), %.3f fps, %.0f Hz audio", g_av.geometry.base_width, g_av.geometry.base_height,
        g_av.geometry.max_width, g_av.geometry.max_height, g_av.timing.fps, g_av.timing.sample_rate);

    const std::string saveStem = gameFile != baseName(romPath) ? stem(gameFile) : stem(romName);
    std::string srm = g_saveDir + "/" + saveStem + ".srm";
    if (size_t n = core.get_memory_size(RETRO_MEMORY_SAVE_RAM)) {
        std::vector<uint8_t> sav;
        if (readFile(srm, sav) && sav.size() == n) {
            std::memcpy(core.get_memory_data(RETRO_MEMORY_SAVE_RAM), sav.data(), n);
            log("loaded save %s", srm.c_str());
        }
    }

    // Pacing. A game that runs at (nearly) the screen's rate is locked to vsync,
    // with the audio stream declared at sample_rate * refresh/fps so one frame
    // of audio lasts exactly one refresh (proven drift-free on the 4KP).
    // Anything else (50 Hz PAL games, odd arcade rates) would play too fast and
    // high-pitched that way, so it is paced by the audio clock at its true rate
    // instead, showing its latest frame on each refresh.
    double fps = g_av.timing.fps > 0 ? g_av.timing.fps : 60.0;
    double refresh = mode.refresh_rate > 0 ? mode.refresh_rate : 60.0;
    double rate = g_av.timing.sample_rate > 0 ? g_av.timing.sample_rate : 44100.0;
    const bool vsyncLock = std::fabs(fps - refresh) / refresh < 0.02;
    // The device runs at its own rate and every conversion happens here, in
    // resampleStereo: the cabinet's older SDL turned odd rates (DoDonPachi's
    // 47997 Hz, speed-matched 48300 Hz) into static when it resampled them.
    const double streamRate = vsyncLock ? rate * refresh / fps : rate;  // what the game's audio must play at
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    SDL_AudioDeviceID audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    const int devRate = audio && have.freq > 0 ? have.freq : want.freq;
    const double baseRatio = devRate / streamRate;  // output frames per game audio frame
    const bool passThrough = !vsyncLock && std::fabs(baseRatio - 1.0) < 1e-6;
    const Uint32 bytesPerSec = (Uint32)devRate * 4;
    // GPU cores (Flycast, PPSSPP) have less even frame times (shader compiles,
    // the read-back), so they keep about 30 ms more audio queued: fewer
    // underruns (crackles) for a delay too small to notice.
    const Uint32 extraMs = g_coreCtx ? 30 : 0;
    const Uint32 cushion = bytesPerSec * (60 + extraMs) / 1000;   // keep ~60 ms queued...
    const Uint32 maxQueue = bytesPerSec * (120 + extraMs) / 1000; // ...and never more than 120 ms
    if (audio) {
        std::vector<uint8_t> silence(cushion, 0);
        SDL_QueueAudio(audio, silence.data(), (Uint32)silence.size());
        SDL_PauseAudioDevice(audio, 0);
        log("audio: game %.0f Hz, played at %.1f Hz, device %d Hz (ratio %.5f), %s pacing (%.3f fps game, %.0f Hz screen)",
            rate, streamRate, devRate, baseRatio, vsyncLock ? "vsync" : "audio-clock", fps, refresh);
    } else {
        log("audio failed: %s (continuing silent)", SDL_GetError());
    }

    // Main loop.
    float aspect = sys->aspect;
    bool sideways = rotate == 90 || rotate == 270;  // the screen (pause menu follows this)
    // The picture also turns as the core asks (vertical arcade games), on top of
    // the screen's own rotation. libretro counts counter-clockwise, SDL clockwise.
    const int picAngle = ((rotate - 90 * (int)g_coreRotation) % 360 + 360) % 360;
    const bool picSideways = picAngle == 90 || picAngle == 270;
    if (picAngle != 0 && bezel.tex) { SDL_DestroyTexture(bezel.tex); bezel.tex = nullptr; }
    log("picture: core rotation %u, screen %d, drawn at %d degrees, frame %ux%u, aspect %.3f", g_coreRotation, rotate,
        picAngle, g_av.geometry.base_width, g_av.geometry.base_height, g_av.geometry.aspect_ratio);
    Uint32 started = SDL_GetTicks(), statsAt = started, startHeld = 0;
    unsigned long statFrames = 0, coreFrames = 0, underruns = 0, catchUps = 0;
    Uint32 lastPresent = SDL_GetTicks();
    SDL_Texture* menuLayer = nullptr;  // pause menu, turned with the game on rotated screens
    const Uint32 drcTarget = bytesPerSec * (50 + extraMs) / 1000;  // hold ~50 ms queued (80 for GPU cores)
    double ratioSum = 0.0;
    unsigned long ratioCount = 0;
    std::string reason;

    // Pause menu / save states. One manual slot plus an automatic one written
    // when you quit, offered back the next time the game starts.
    const std::string statePath = g_saveDir + "/" + saveStem + ".state";
    const std::string autoPath = g_saveDir + "/" + saveStem + ".auto.state";
    bool canState = false;
    { CoreGL glScope; canState = core.serialize_size && core.serialize && core.unserialize && core.serialize_size() > 0; }
    enum class Menu { None, Pause, Continue, Options } menu = Menu::None;
    int optSel = 0, optTop = 0;
    const int optVisible = 8;
    const std::string coreTitle = std::string(info.library_name ? info.library_name : "Core") + " " +
                                  (info.library_version ? info.library_version : "");
    int menuSel = 0;
    std::string toast;
    Uint32 toastAt = 0;
    uint16_t prevButtons = 0;
    bool prevGuide = false, suppressInput = false;
    // A state only loads in the core that wrote it (a QuickNES state means
    // nothing to FCEUmm), so each one records its core in "<state>.core".
    // States without that record predate it and are treated as foreign.
    const std::string coreTag = std::string(info.library_name ? info.library_name : "?") + " " +
                                (info.library_version ? info.library_version : "");
    auto stateMatches = [&](const std::string& path) {
        std::vector<uint8_t> tag;
        return fileExists(path) && readFile(path + ".core", tag) && std::string(tag.begin(), tag.end()) == coreTag;
    };
    auto saveState = [&](const std::string& path) {
        if (!canState) return false;
        CoreGL glScope;
        std::vector<uint8_t> buf(core.serialize_size());
        bool ok = core.serialize(buf.data(), buf.size()) && writeFile(path, buf.data(), buf.size()) &&
                  writeFile(path + ".core", coreTag.data(), coreTag.size());
        log("save state %s: %s", path.c_str(), ok ? "ok" : "failed");
        return ok;
    };
    auto loadState = [&](const std::string& path) {
        std::vector<uint8_t> buf;
        CoreGL glScope;
        bool ok = canState && readFile(path, buf) && core.unserialize(buf.data(), buf.size());
        log("load state %s: %s", path.c_str(), ok ? "ok" : "failed");
        return ok;
    };
    auto pauseAudio = [&](bool pause) {
        if (!audio) return;
        if (pause) {
            SDL_PauseAudioDevice(audio, 1);
            SDL_ClearQueuedAudio(audio);
        } else {
            std::vector<uint8_t> silence(cushion, 0);
            SDL_QueueAudio(audio, silence.data(), (Uint32)silence.size());
            g_drcHavePrev = false;
            SDL_PauseAudioDevice(audio, 0);
        }
    };
    auto openMenu = [&](Menu m) {
        menu = m;
        menuSel = 0;
        toast.clear();
        pauseAudio(true);
    };
    auto closeMenu = [&]() {
        menu = Menu::None;
        suppressInput = true;  // don't hand the confirming press to the game
        startHeld = 0;
        pauseAudio(false);
    };
    if (canState && fileExists(autoPath) && !stateMatches(autoPath))
        log("not offering %s: saved with a different core", autoPath.c_str());
    if (canState && stateMatches(autoPath)) {
        // Show the game's first frame behind the question. Some cores (FBNeo)
        // send no picture on their first frame or two, and with nothing to draw
        // the question would be invisible, so run until there is one (at most
        // a second; whatever happens next is replaced by the state or a reset).
        for (int i = 0; i < 60 && !g_texture; ++i) {
            g_audioBatch.clear();
            { CoreGL glScope; core.run(); }
            flushHwFrame();
        }
        g_audioBatch.clear();
        openMenu(Menu::Continue);
        log("offering to continue from %s", autoPath.c_str());
    }

    while (reason.empty()) {
        // A single frame taking 10 s means the core is stuck. GPU cores get longer
        // at first: the Mali compiles shaders as new scenes appear.
        ::alarm(g_coreCtx && coreFrames < 900 ? 30 : 10);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) reason = "quit";
            if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED) openPads();
        }
        // Menus (and hold-Start for the pause menu) always use the default
        // layout, so a remap can't lock anyone out; the game gets its own.
        uint16_t raw = readButtons(Library::defaultButtonMap());
        uint16_t gameRaw = readButtons(buttonMap);
        uint16_t down = raw & ~prevButtons;
        prevButtons = raw;
        bool guide = padButton(SDL_CONTROLLER_BUTTON_GUIDE), guideDown = guide && !prevGuide;
        prevGuide = guide;
        Uint32 t = SDL_GetTicks();
        auto pressed = [&](int id) { return (down >> id) & 1; };

        std::vector<std::string> items;
        std::vector<bool> enabled;
        if (menu == Menu::Pause) {
            items = {"Resume", "Save state", "Load state", "Reset", "Core options", "Quit to menu"};
            enabled = {true, canState, canState && stateMatches(statePath), core.reset != nullptr, !g_optDefs.empty(), true};
        } else if (menu == Menu::Continue) {
            items = {"Continue where you left off", "Start from the beginning"};
            enabled = {true, true};
        }

        if (menu == Menu::None) {
            // While the game runs: Home, or Start held a second, opens the menu.
            if (suppressInput && raw == 0) suppressInput = false;
            g_buttons = suppressInput ? 0 : gameRaw;
            // Analog: the stick if it's pushed, else the D-pad at full tilt (the
            // cabinet's joystick may report as a D-pad; PSP games often read only
            // the analog nub).
            {
                int lx = padAxis(SDL_CONTROLLER_AXIS_LEFTX), ly = padAxis(SDL_CONTROLLER_AXIS_LEFTY);
                auto bit = [&](int id) { return (g_buttons >> id) & 1; };
                if (std::abs(lx) < 8000) lx = bit(RETRO_DEVICE_ID_JOYPAD_RIGHT) ? 32767 : bit(RETRO_DEVICE_ID_JOYPAD_LEFT) ? -32767 : 0;
                if (std::abs(ly) < 8000) ly = bit(RETRO_DEVICE_ID_JOYPAD_DOWN) ? 32767 : bit(RETRO_DEVICE_ID_JOYPAD_UP) ? -32767 : 0;
                g_analog[0][0] = suppressInput ? 0 : (int16_t)lx;
                g_analog[0][1] = suppressInput ? 0 : (int16_t)ly;
                g_analog[1][0] = suppressInput ? 0 : (int16_t)padAxis(SDL_CONTROLLER_AXIS_RIGHTX);
                g_analog[1][1] = suppressInput ? 0 : (int16_t)padAxis(SDL_CONTROLLER_AXIS_RIGHTY);
            }
            if (raw & (1u << RETRO_DEVICE_ID_JOYPAD_START)) {
                if (!startHeld) startHeld = t;
                else if (t - startHeld >= 1000) openMenu(Menu::Pause);
            } else {
                startHeld = 0;
            }
            if (guideDown) openMenu(Menu::Pause);
        } else if (menu == Menu::Options) {
            g_buttons = 0;
            std::memset(g_analog, 0, sizeof(g_analog));
            int n = (int)g_optDefs.size();
            auto change = [&](int dir) {
                const OptDef& d = g_optDefs[optSel];
                auto it = std::find(d.values.begin(), d.values.end(), g_options[d.key]);
                int i = it == d.values.end() ? 0 : (int)(it - d.values.begin());
                int m = (int)d.values.size();
                if (m > 0) setOption(d.key, d.values[(i + dir + m) % m]);
            };
            if (n == 0 || pressed(RETRO_DEVICE_ID_JOYPAD_A) || guideDown || pressed(RETRO_DEVICE_ID_JOYPAD_START)) {
                menu = Menu::Pause;  // back to the pause menu, on "Core options"
                menuSel = 4;
            } else {
                if (pressed(RETRO_DEVICE_ID_JOYPAD_UP)) optSel = (optSel + n - 1) % n;
                if (pressed(RETRO_DEVICE_ID_JOYPAD_DOWN)) optSel = (optSel + 1) % n;
                if (pressed(RETRO_DEVICE_ID_JOYPAD_L)) optSel = std::max(0, optSel - optVisible);
                if (pressed(RETRO_DEVICE_ID_JOYPAD_R)) optSel = std::min(n - 1, optSel + optVisible);
                if (pressed(RETRO_DEVICE_ID_JOYPAD_LEFT)) change(-1);
                if (pressed(RETRO_DEVICE_ID_JOYPAD_RIGHT) || pressed(RETRO_DEVICE_ID_JOYPAD_B)) change(1);
                if (optSel < optTop) optTop = optSel;
                if (optSel >= optTop + optVisible) optTop = optSel - optVisible + 1;
            }
        } else {
            g_buttons = 0;
            std::memset(g_analog, 0, sizeof(g_analog));
            int n = (int)items.size();
            if (pressed(RETRO_DEVICE_ID_JOYPAD_UP)) menuSel = (menuSel + n - 1) % n;
            if (pressed(RETRO_DEVICE_ID_JOYPAD_DOWN)) menuSel = (menuSel + 1) % n;
            // Cabinet A arrives as libretro B, cabinet B as libretro A.
            bool confirm = pressed(RETRO_DEVICE_ID_JOYPAD_B);
            bool back = pressed(RETRO_DEVICE_ID_JOYPAD_A) || guideDown;
            if (menu == Menu::Pause && (back || (pressed(RETRO_DEVICE_ID_JOYPAD_START) && !confirm))) {
                closeMenu();
            } else if (confirm && !enabled[menuSel]) {
                toast = menuSel == 4              ? "This emulator has no options"
                        : menuSel != 2            ? "This emulator can't do that"
                        : fileExists(statePath) ? "Saved with a different emulator"
                                                : "No saved state yet";
                toastAt = t;
            } else if (confirm && menu == Menu::Continue) {
                if (menuSel == 0) loadState(autoPath);
                else { ::unlink(autoPath.c_str()); if (core.reset) { CoreGL glScope; core.reset(); } }
                closeMenu();
            } else if (confirm) {
                switch (menuSel) {
                case 0: closeMenu(); break;
                case 1: toast = saveState(statePath) ? "State saved" : "Could not save the state"; toastAt = t; break;
                case 2:
                    if (loadState(statePath)) closeMenu();
                    else { toast = "Could not load the state"; toastAt = t; }
                    break;
                case 3: { CoreGL glScope; core.reset(); } closeMenu(); break;
                case 4: menu = Menu::Options; optSel = optTop = 0; break;
                case 5:
                    if (canState) saveState(autoPath);  // resume here next time
                    reason = "menu";
                    break;
                }
            }
            if (!toast.empty() && t - toastAt > 2500) toast.clear();
        }

        auto runFrame = [&]() {
            g_audioBatch.clear();
            { CoreGL glScope; core.run(); }
            flushHwFrame();
            ++coreFrames;
            if (audio && !g_audioBatch.empty()) {
                Uint32 queued = SDL_GetQueuedAudioSize(audio);
                if (queued == 0) ++underruns;
                if (passThrough) {
                    SDL_QueueAudio(audio, g_audioBatch.data(), (Uint32)(g_audioBatch.size() * sizeof(int16_t)));
                } else {
                    double drc = 1.0;
                    if (vsyncLock) {  // nudge toward ~50 ms queued (vsync and audio clocks differ slightly)
                        double err = ((double)drcTarget - queued) / drcTarget;  // + = running low
                        drc = 1.0 + 0.005 * std::max(-1.0, std::min(1.0, err));
                        ratioSum += drc;
                        ++ratioCount;
                    }
                    const std::vector<int16_t>& out = resampleStereo(g_audioBatch, baseRatio * drc);
                    SDL_QueueAudio(audio, out.data(), (Uint32)(out.size() * sizeof(int16_t)));
                }
            }
        };
        if (menu != Menu::None) {
            // Frozen while the menu is open.
        } else if (vsyncLock || !audio) {
            runFrame();
            // Running low (a slow frame, a log write): emulate one extra frame
            // to refill the audio; its picture is simply superseded.
            if (audio && SDL_GetQueuedAudioSize(audio) < bytesPerSec * 15 / 1000) { runFrame(); ++catchUps; }
        } else {
            for (int i = 0; i < 3 && SDL_GetQueuedAudioSize(audio) < cushion; ++i) runFrame();
        }

        // Present every refresh (vsync paces the loop), so the bar animation
        // stays smooth even when the game itself runs at 50 fps.
        bool presented = false;
        if (g_texture && g_frameW > 0) {
            presented = true;
            g_newFrame = false;
            // Crop blank edge columns; the display aspect shrinks with them.
            int cropL = std::min(g_cropL, g_frameW / 4), cropR = std::min(g_cropR, g_frameW / 4);
            SDL_Rect src{cropL, 0, g_frameW - cropL - cropR, g_frameH};
            float shownAspect = aspect * (float)src.w / (float)g_frameW;
            // The area the picture fits in: the bezel's window, or the screen.
            // Arcade games give their own shape (and may change it).
            // The core's aspect ratio is the picture as shown, after its rotation
            // (as RetroArch reads it), so a frame that still has to turn a
            // quarter has the inverse shape.
            if ((Library::isArcadeSystem(sys->id) || g_coreCtx) && g_av.geometry.base_height > 0) {
                aspect = g_av.geometry.aspect_ratio > 0 ? g_av.geometry.aspect_ratio
                                                        : (float)g_av.geometry.base_width / g_av.geometry.base_height;
                if ((g_coreRotation & 1) && g_av.geometry.aspect_ratio > 0) aspect = 1.0f / aspect;
            }
            float bx = 0, by = 0, bw = picSideways ? winH : winW, bh = picSideways ? winW : winH;
            if (bezel.tex) {
                float sx = (float)winW / bezel.w, sy = (float)winH / bezel.h;
                bx = bezel.window.x * sx; by = bezel.window.y * sy;
                bw = bezel.window.w * sx; bh = bezel.window.h * sy;
            }
            float cw = std::min(bw, bh * shownAspect), ch = cw / shownAspect;
            if (scaling == "integer") {
                // Pixel-perfect: a whole-number multiple of the game's lines.
                // (Height in the picture's own orientation: ch for upright, cw
                // is the matching width at the right shape.)
                int mult = (int)(ch / src.h);
                if (mult >= 1) { ch = (float)(mult * src.h); cw = ch * shownAspect; }
            }
            SDL_Rect dst{(int)(bx + (bw - cw) / 2), (int)(by + (bh - ch) / 2), (int)cw, (int)ch};
            if (!bezel.tex) dst = SDL_Rect{(int)((winW - cw) / 2), (int)((winH - ch) / 2), (int)cw, (int)ch};
            // The picture's on-screen footprint after rotation, for the bars.
            SDL_Rect shown = picSideways ? SDL_Rect{(int)((winW - ch) / 2), (int)((winH - cw) / 2), (int)ch, (int)cw} : dst;
            float frameDt = std::min(0.1f, (t - lastPresent) / 1000.0f);
            lastPresent = t;
            if (bezel.tex) {
                SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
                SDL_RenderClear(g_renderer);
            } else {
                g_ambient.draw(shown, winW, winH, frameDt);
            }
            SDL_RenderCopyEx(g_renderer, g_texture, &src, &dst, picAngle, nullptr, SDL_FLIP_NONE);
            if (scanDark > 0) {
                if (!scanLines) {  // once: at the game's line count when first shown
                    scanLines = std::max(1, src.h);
                    scanTex = makeScanlines(g_renderer, scanLines, scanDark);
                }
                if (scanTex) {
                    // Only as many lines as the game shows now (it can change mode).
                    SDL_Rect ss{0, 0, 64, std::min(scanLines, src.h) * 2};
                    SDL_RenderCopyEx(g_renderer, scanTex, &ss, &dst, picAngle, nullptr, SDL_FLIP_NONE);
                }
            }
            if (bezel.tex) SDL_RenderCopy(g_renderer, bezel.tex, nullptr, nullptr);
            if (menu != Menu::None) {
                const char* title = menu == Menu::Continue ? "WELCOME BACK" : "PAUSED";
                auto drawMenuAt = [&](int w, int h) {
                    if (menu == Menu::Options) drawOptionsMenu(g_renderer, w, h, coreTitle, optSel, optTop, optVisible);
                    else drawPauseMenu(g_renderer, w, h, title, items, enabled, menuSel, toast);
                };
                if (!sideways && rotate == 0) {
                    drawMenuAt(winW, winH);
                } else {
                    // Draw upright on a layer the size of the player's view,
                    // then turn it exactly like the game picture.
                    int tw = sideways ? winH : winW, th = sideways ? winW : winH;
                    if (!menuLayer) {
                        menuLayer = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, tw, th);
                        if (menuLayer) SDL_SetTextureBlendMode(menuLayer, SDL_BLENDMODE_BLEND);
                        log("pause menu layer %dx%d (rotate %d): %s", tw, th, rotate, menuLayer ? "ok" : SDL_GetError());
                    }
                    if (menuLayer && SDL_SetRenderTarget(g_renderer, menuLayer) == 0) {
                        SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 0);
                        SDL_RenderClear(g_renderer);
                        drawMenuAt(tw, th);
                        SDL_SetRenderTarget(g_renderer, nullptr);
                        SDL_Rect md{(winW - tw) / 2, (winH - th) / 2, tw, th};
                        SDL_RenderCopyEx(g_renderer, menuLayer, nullptr, &md, rotate, nullptr, SDL_FLIP_NONE);
                    } else {
                        drawMenuAt(winW, winH);
                    }
                }
            }
            SDL_RenderPresent(g_renderer);
            static bool firstShown = false;
            if (!firstShown) { firstShown = true; log("first frame shown"); }
        }
        if (!presented) SDL_Delay(2);
        while (audio && SDL_GetQueuedAudioSize(audio) > maxQueue) SDL_Delay(1);

        if (t - statsAt >= 10000) {
            log("stats: %.1f game fps, frame %dx%d, audio %u ms, rate %+.3f%%, underruns %lu, catch-ups %lu, dupes %lu",
                (coreFrames - statFrames) * 1000.0 / (t - statsAt), g_frameW, g_frameH,
                audio ? SDL_GetQueuedAudioSize(audio) * 1000 / bytesPerSec : 0,
                ratioCount ? (ratioSum / ratioCount - 1.0) * 100.0 : 0.0, underruns, catchUps, g_dupes);
            ratioSum = 0.0;
            ratioCount = 0;
            statsAt = t;
            statFrames = coreFrames;
        }
    }
    ::alarm(30);  // still guarded while saving and shutting down
    log("stopping (%s) after %u s, %lu frames", reason.c_str(), (SDL_GetTicks() - started) / 1000, g_frames);

    if (size_t n = core.get_memory_size(RETRO_MEMORY_SAVE_RAM)) {
        if (writeFile(srm, core.get_memory_data(RETRO_MEMORY_SAVE_RAM), n)) log("saved %s", srm.c_str());
        else log("could not save %s: %s", srm.c_str(), std::strerror(errno));
    }
    {
        CoreGL glScope;
        core.unload_game();
        if (g_coreCtx && g_hw.context_destroy) g_hw.context_destroy();
        core.deinit();
    }
    if (g_readbacks) log("gpu: %lu frames read back, %.2f ms each", g_readbacks, g_readbackMs / g_readbacks);
    if (audio) SDL_CloseAudioDevice(audio);
    g_ambient.shutdown();
    Gfx::shutdown();
    if (bezel.tex) SDL_DestroyTexture(bezel.tex);
    if (scanTex) SDL_DestroyTexture(scanTex);
    if (menuLayer) SDL_DestroyTexture(menuLayer);
    panels.reset();  // join its worker and release buffers before SDL closes the fd
    if (g_texture) SDL_DestroyTexture(g_texture);
    SDL_DestroyRenderer(g_renderer);
    SDL_DestroyWindow(window);
    for (SDL_GameController* p : g_pads) SDL_GameControllerClose(p);
    SDL_Quit();
    if (romFile != romPath) ::unlink(romFile.c_str());
    ::alarm(0);
    Library::execMenu(appDir, menuSys, menuIndex, "");
    return 0;
}
