#include "GamePanels.h"

#include "DrmKms.h"
#include "Library.h"
#include "PanelFont.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "vendor/stb_image.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using Library::log;

constexpr uint8_t kInk[3] = {245, 246, 250};
constexpr uint8_t kGold[3] = {245, 214, 95};
constexpr uint8_t kTeal[3] = {43, 199, 184};
constexpr uint8_t kViolet[3] = {124, 92, 232};
constexpr uint8_t kWhite[3] = {255, 255, 255};
constexpr uint8_t kGradTop[3] = {16, 30, 46};
constexpr uint8_t kGradBot[3] = {30, 16, 50};

int findCard0Fd() {
    DIR* d = ::opendir("/proc/self/fd");
    if (!d) return -1;
    int best = -1;
    while (dirent* e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;
        char path[64], target[256];
        std::snprintf(path, sizeof(path), "/proc/self/fd/%s", e->d_name);
        ssize_t n = ::readlink(path, target, sizeof(target) - 1);
        if (n <= 0) continue;
        target[n] = 0;
        if (std::strstr(target, "/dev/dri/card")) { best = std::atoi(e->d_name); break; }
    }
    ::closedir(d);
    return best;
}

void gradient(std::vector<uint8_t>& c, int W, int H, const uint8_t top[3], const uint8_t bot[3]) {
    for (int y = 0; y < H; ++y) {
        float t = H > 1 ? (float)y / (H - 1) : 0.0f;
        uint8_t r = (uint8_t)(top[0] + (bot[0] - top[0]) * t), g = (uint8_t)(top[1] + (bot[1] - top[1]) * t),
                b = (uint8_t)(top[2] + (bot[2] - top[2]) * t);
        uint8_t* row = &c[(size_t)y * W * 4];
        for (int x = 0; x < W; ++x) { row[x * 4] = r; row[x * 4 + 1] = g; row[x * 4 + 2] = b; row[x * 4 + 3] = 255; }
    }
}

// A soft radial pool of light (additive), for the no-art title card.
void glow(std::vector<uint8_t>& c, int W, int H, float cx, float cy, float radius, const uint8_t col[3], float strength) {
    int x0 = std::max(0, (int)(cx - radius)), x1 = std::min(W, (int)(cx + radius));
    int y0 = std::max(0, (int)(cy - radius)), y1 = std::min(H, (int)(cy + radius));
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) / radius;
            if (d >= 1.0f) continue;
            float a = (1.0f - d) * (1.0f - d) * strength;
            uint8_t* p = &c[((size_t)y * W + x) * 4];
            for (int k = 0; k < 3; ++k) p[k] = (uint8_t)std::min(255.0f, p[k] + col[k] * a);
        }
}

void text(std::vector<uint8_t>& c, int W, int H, const std::string& s, float cx, float top, float px,
          const uint8_t col[3], PanelFont::Face face, float maxW) {
    if (s.empty()) return;
    float w = PanelFont::measure(s, px, face);
    if (w > maxW && w > 0) { px *= maxW / w; w = maxW; }
    PanelFont::draw(c.data(), W, H, s, cx - w * 0.5f, top, px, col[0], col[1], col[2], face);
}

// Blurred, darkened backdrop: average to a tiny grid, bilinear back up.
void backdrop(std::vector<uint8_t>& c, int W, int H, const std::vector<uint8_t>& img, int iw, int ih) {
    constexpr int SB = 18;
    float small[SB * SB * 3];
    for (int sy = 0; sy < SB; ++sy)
        for (int sx = 0; sx < SB; ++sx) {
            int x0 = sx * iw / SB, x1 = std::max(x0 + 1, (sx + 1) * iw / SB);
            int y0 = sy * ih / SB, y1 = std::max(y0 + 1, (sy + 1) * ih / SB);
            float r = 0, g = 0, b = 0;
            int n = 0;
            for (int y = y0; y < y1; y += 2)
                for (int x = x0; x < x1; x += 2) {
                    const uint8_t* s = &img[((size_t)y * iw + x) * 4];
                    r += s[0]; g += s[1]; b += s[2]; ++n;
                }
            float* d = &small[(sy * SB + sx) * 3];
            d[0] = r / n; d[1] = g / n; d[2] = b / n;
        }
    const float dim = 0.36f;
    for (int y = 0; y < H; ++y) {
        float fv = (y + 0.5f) / H * SB - 0.5f;
        int v0 = (int)std::floor(fv);
        float vt = fv - v0;
        int a0 = std::min(std::max(v0, 0), SB - 1), a1 = std::min(std::max(v0 + 1, 0), SB - 1);
        for (int x = 0; x < W; ++x) {
            float fu = (x + 0.5f) / W * SB - 0.5f;
            int u0 = (int)std::floor(fu);
            float ut = fu - u0;
            int b0 = std::min(std::max(u0, 0), SB - 1), b1 = std::min(std::max(u0 + 1, 0), SB - 1);
            uint8_t* o = &c[((size_t)y * W + x) * 4];
            for (int k = 0; k < 3; ++k) {
                float t = small[(a0 * SB + b0) * 3 + k] * (1 - ut) + small[(a0 * SB + b1) * 3 + k] * ut;
                float bo = small[(a1 * SB + b0) * 3 + k] * (1 - ut) + small[(a1 * SB + b1) * 3 + k] * ut;
                o[k] = (uint8_t)((t * (1 - vt) + bo * vt) * dim);
            }
            o[3] = 255;
        }
    }
}

// Bilinear, aspect-fit blit of an RGBA image into a box, with a soft shadow.
void blitFit(std::vector<uint8_t>& c, int W, int H, const std::vector<uint8_t>& img, int iw, int ih, int bx, int by,
             int bw, int bh) {
    float s = std::min((float)bw / iw, (float)bh / ih);
    int dw = std::max(1, (int)(iw * s)), dh = std::max(1, (int)(ih * s));
    int ox = bx + (bw - dw) / 2, oy = by + (bh - dh) / 2;
    // shadow (only for opaque pictures; a cut-out console casts none)
    bool opaque = true;
    for (size_t i = 3; i < img.size() && opaque; i += 4 * 97) opaque = img[i] > 250;
    const int sh = opaque ? std::max(8, H / 60) : 0;
    for (int y = oy - sh; sh > 0 && y < oy + dh + sh * 2; ++y)
        for (int x = ox - sh; x < ox + dw + sh * 2; ++x) {
            if (x < 0 || y < 0 || x >= W || y >= H) continue;
            float dx = std::max(0, std::max(ox - x, x - (ox + dw - 1) - sh));
            float dy = std::max(0, std::max(oy - y, y - (oy + dh - 1) - sh));
            float d = std::sqrt(dx * dx + dy * dy) / (sh * 1.5f);
            if (d >= 1.0f) continue;
            uint8_t* p = &c[((size_t)y * W + x) * 4];
            float k = 1.0f - 0.55f * (1.0f - d);
            p[0] = (uint8_t)(p[0] * k); p[1] = (uint8_t)(p[1] * k); p[2] = (uint8_t)(p[2] * k);
        }
    for (int y = 0; y < dh; ++y) {
        int cy = oy + y;
        if (cy < 0 || cy >= H) continue;
        float fv = (y + 0.5f) / dh * ih - 0.5f;
        int v0 = std::min(std::max((int)std::floor(fv), 0), ih - 1), v1 = std::min(v0 + 1, ih - 1);
        float vt = std::max(0.0f, fv - v0);
        for (int x = 0; x < dw; ++x) {
            int cx = ox + x;
            if (cx < 0 || cx >= W) continue;
            float fu = (x + 0.5f) / dw * iw - 0.5f;
            int u0 = std::min(std::max((int)std::floor(fu), 0), iw - 1), u1 = std::min(u0 + 1, iw - 1);
            float ut = std::max(0.0f, fu - u0);
            const uint8_t *s00 = &img[((size_t)v0 * iw + u0) * 4], *s10 = &img[((size_t)v0 * iw + u1) * 4],
                          *s01 = &img[((size_t)v1 * iw + u0) * 4], *s11 = &img[((size_t)v1 * iw + u1) * 4];
            uint8_t* q = &c[((size_t)cy * W + cx) * 4];
            float a = ((s00[3] * (1 - ut) + s10[3] * ut) * (1 - vt) + (s01[3] * (1 - ut) + s11[3] * ut) * vt) / 255.0f;
            for (int k = 0; k < 3; ++k) {
                float v = (s00[k] * (1 - ut) + s10[k] * ut) * (1 - vt) + (s01[k] * (1 - ut) + s11[k] * ut) * vt;
                q[k] = (uint8_t)(v * a + q[k] * (1 - a));
            }
        }
    }
}

// ---- placeholder cover --------------------------------------------------------
// Until real box art exists, each game gets a generated cover in the same layout
// real art uses: colours picked from its name (stable between visits), a system
// banner, the title in the display face, and a faint giant initial behind it.

uint32_t fnv(const std::string& s) {
    uint32_t h = 2166136261u;
    for (char c : s) h = (h ^ (uint8_t)c) * 16777619u;
    return h;
}

void mixRgb(const uint8_t a[3], const uint8_t b[3], float t, uint8_t out[3]) {
    for (int k = 0; k < 3; ++k) out[k] = (uint8_t)(a[k] + (b[k] - a[k]) * t);
}

// Splits a title into at most `maxLines` lines that fit `maxW` at `px`.
std::vector<std::string> wrap(const std::string& text, float px, float maxW, int maxLines) {
    std::vector<std::string> words, lines;
    std::string w;
    for (char c : text + " ") {
        if (c == ' ') { if (!w.empty()) words.push_back(w); w.clear(); }
        else w += c;
    }
    std::string line;
    for (const std::string& word : words) {
        std::string trial = line.empty() ? word : line + " " + word;
        if (!line.empty() && PanelFont::measure(trial, px, PanelFont::Face::Display) > maxW) {
            lines.push_back(line);
            line = word;
        } else {
            line = trial;
        }
    }
    if (!line.empty()) lines.push_back(line);
    if ((int)lines.size() > maxLines) {
        lines.resize(maxLines);
        lines.back() += "...";
    }
    return lines;
}

std::vector<uint8_t> makeCover(const std::string& system, const std::string& title, int& outW, int& outH) {
    const int W = 600, H = 840;
    outW = W; outH = H;
    std::vector<uint8_t> c((size_t)W * H * 4, 255);
    static const uint8_t pal[5][3] = {{43, 199, 184}, {124, 92, 232}, {245, 214, 95}, {239, 92, 120}, {70, 140, 240}};
    uint32_t h = fnv(title);
    const uint8_t* a = pal[h % 5];
    const uint8_t* b = pal[(h / 5 + 1 + h % 4) % 5];
    const uint8_t dark[3] = {10, 12, 22};
    uint8_t top[3], bot[3], band[3];
    mixRgb(a, dark, 0.35f, top);
    mixRgb(b, dark, 0.72f, bot);
    mixRgb(pal[fnv(system) % 5], dark, 0.15f, band);

    // Body: diagonal-ish gradient with soft light streaks.
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float t = std::min(1.0f, std::max(0.0f, (y * 0.8f + x * 0.35f) / (H * 0.8f + W * 0.35f)));
            float streak = 0.5f + 0.5f * std::sin((x * 0.9f - y) * 0.018f + (h % 7));
            uint8_t* p = &c[((size_t)y * W + x) * 4];
            for (int k = 0; k < 3; ++k)
                p[k] = (uint8_t)std::min(255.0f, top[k] + (bot[k] - top[k]) * t + streak * 10.0f);
            p[3] = 255;
        }
    // Giant faint initial.
    char initial = '#';
    for (char ch : title)
        if (std::isalnum((unsigned char)ch)) { initial = (char)std::toupper((unsigned char)ch); break; }
    std::string ini(1, initial);
    float ipx = H * 0.9f;
    float iw = PanelFont::measure(ini, ipx, PanelFont::Face::Display);
    uint8_t ghost[3];
    mixRgb(top, kWhite, 0.10f, ghost);
    PanelFont::draw(c.data(), W, H, ini, W * 0.62f - iw * 0.5f, H * 0.10f, ipx, ghost[0], ghost[1], ghost[2],
                    PanelFont::Face::Display);

    // System banner.
    const int bandH = 96;
    for (int y = 0; y < bandH; ++y)
        for (int x = 0; x < W; ++x) {
            uint8_t* p = &c[((size_t)y * W + x) * 4];
            float shade = 1.0f - 0.25f * y / bandH;
            for (int k = 0; k < 3; ++k) p[k] = (uint8_t)(band[k] * shade);
        }
    for (int x = 0; x < W; ++x)
        for (int y = bandH; y < bandH + 4; ++y) {
            uint8_t* p = &c[((size_t)y * W + x) * 4];
            p[0] = 245; p[1] = 246; p[2] = 250;
        }
    std::string sys = system;
    for (char& ch : sys) ch = (char)std::toupper((unsigned char)ch);
    float spx = 58.0f, sw = PanelFont::measure(sys, spx, PanelFont::Face::Display);
    if (sw > W * 0.86f) { spx *= W * 0.86f / sw; sw = W * 0.86f; }
    PanelFont::draw(c.data(), W, H, sys, (W - sw) * 0.5f, (bandH - spx) * 0.5f - 4.0f, spx, 255, 255, 255,
                    PanelFont::Face::Display);

    // Title, wrapped and centred in the lower body, with a drop shadow.
    float tpx = 112.0f;
    std::vector<std::string> lines = wrap(title, tpx, W * 0.84f, 4);
    while (tpx > 56.0f && (lines.size() > 3 || [&] {
               for (auto& l : lines) if (PanelFont::measure(l, tpx, PanelFont::Face::Display) > W * 0.84f) return true;
               return false;
           }())) {
        tpx -= 8.0f;
        lines = wrap(title, tpx, W * 0.84f, 4);
    }
    float lineH = tpx * 1.02f;
    float y0 = H * 0.70f - lines.size() * lineH * 0.5f;
    for (size_t i = 0; i < lines.size(); ++i) {
        float lw = PanelFont::measure(lines[i], tpx, PanelFont::Face::Display);
        float x = (W - lw) * 0.5f, y = y0 + i * lineH;
        PanelFont::draw(c.data(), W, H, lines[i], x + 5, y + 6, tpx, 0, 0, 0, PanelFont::Face::Display);
        PanelFont::draw(c.data(), W, H, lines[i], x, y, tpx, 255, 255, 255, PanelFont::Face::Display);
    }

    // Thin light edge.
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (x < 3 || y < 3 || x >= W - 3 || y >= H - 3) {
                uint8_t* p = &c[((size_t)y * W + x) * 4];
                p[0] = 230; p[1] = 234; p[2] = 242;
            }
    return c;
}

bool isFile(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

} // namespace

std::string findConsoleArt(const std::string& appDir, const std::string& system) {
    for (const char* ext : {".png", ".jpg"}) {
        std::string p = appDir + "/media/" + system + "/console" + ext;
        if (isFile(p)) return p;
    }
    return "";
}

namespace {

// Product photos usually sit on white. For an image with no transparency,
// flood-fill near-white pixels connected to the border to transparent, and
// soften the pixels bordering them, so the console floats on the panel.
void keyOutWhite(std::vector<uint8_t>& img, int w, int h) {
    for (size_t i = 3; i < img.size(); i += 4)
        if (img[i] < 250) return;  // already has transparency
    auto whiteish = [&](int x, int y) {
        const uint8_t* p = &img[((size_t)y * w + x) * 4];
        int mn = std::min(p[0], std::min(p[1], p[2])), mx = std::max(p[0], std::max(p[1], p[2]));
        return mn > 222 && mx - mn < 28;
    };
    std::vector<uint8_t> bg((size_t)w * h, 0);
    std::vector<int> stack;
    auto push = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        size_t i = (size_t)y * w + x;
        if (bg[i] || !whiteish(x, y)) return;
        bg[i] = 1;
        stack.push_back((int)i);
    };
    for (int x = 0; x < w; ++x) { push(x, 0); push(x, h - 1); }
    for (int y = 0; y < h; ++y) { push(0, y); push(w - 1, y); }
    while (!stack.empty()) {
        int i = stack.back();
        stack.pop_back();
        int x = i % w, y = i / w;
        push(x + 1, y); push(x - 1, y); push(x, y + 1); push(x, y - 1);
    }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            size_t i = (size_t)y * w + x;
            if (bg[i]) { img[i * 4 + 3] = 0; continue; }
            // Edge pixel next to the background: fade by how white it is.
            bool edge = (x > 0 && bg[i - 1]) || (x + 1 < w && bg[i + 1]) || (y > 0 && bg[i - w]) || (y + 1 < h && bg[i + w]);
            if (edge) {
                const uint8_t* p = &img[i * 4];
                int mn = std::min(p[0], std::min(p[1], p[2]));
                img[i * 4 + 3] = (uint8_t)std::max(60, 255 - std::max(0, mn - 160) * 2);
            }
        }
}

bool loadImage(const std::string& path, std::vector<uint8_t>& out, int& w, int& h) {
    int n = 0;
    uint8_t* px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!px) return false;
    out.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    return true;
}

} // namespace

std::string findArt(const std::string& appDir, const std::string& system, const std::string& romFile) {
    std::string stem = romFile.substr(0, romFile.find_last_of('.'));
    // libretro-thumbnails replaces these characters with '_' in file names.
    std::string safe = stem;
    for (char& ch : safe)
        if (std::strchr("&*/:`<>?\\|\"", ch)) ch = '_';
    static const char* kFolders[] = {"boxart", "Named_Boxarts", "snaps", "Named_Snaps", "titles", "Named_Titles"};
    std::string base = appDir + "/media/" + system + "/";
    for (const char* f : kFolders)
        for (const std::string* name : {&stem, &safe})
            for (const char* ext : {".png", ".jpg"}) {
                std::string p = base + f + "/" + *name + ext;
                if (isFile(p)) return p;
            }
    return "";
}

GamePanels::~GamePanels() {
    // Stop the worker before releasing the buffers it draws into (and before
    // SDL closes the fd they live on; Menu::shutdown destroys us first).
    if (m_thread.joinable()) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_one();
        m_thread.join();
    }
    for (Panel& p : m_panels) {
        if (p.map) ::munmap(p.map, p.size);
        if (p.fbId) ::ioctl(m_fd, DRM_IOCTL_MODE_RMFB, &p.fbId);
        if (p.handle) {
            drm_mode_destroy_dumb dd;
            std::memset(&dd, 0, sizeof(dd));
            dd.handle = p.handle;
            ::ioctl(m_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
        }
    }
}

// The CRTC to drive a connector with. Normally the one its current encoder is
// bound to; but once a screen has been switched off (our buffers are released
// when the menu hands over to a game) the connector can come back with no
// encoder, so fall back to a CRTC one of its encoders can use that isn't
// driving the playfield or a panel we already claimed.
uint32_t GamePanels::pickCrtc(uint32_t currentEncoder, const std::vector<uint32_t>& encoders, uint32_t count,
                              uint32_t mainConnector) {
    if (currentEncoder) {
        drm_mode_get_encoder en;
        std::memset(&en, 0, sizeof(en));
        en.encoder_id = currentEncoder;
        if (::ioctl(m_fd, DRM_IOCTL_MODE_GETENCODER, &en) == 0 && en.crtc_id) return en.crtc_id;
    }
    drm_mode_card_res res;
    std::memset(&res, 0, sizeof(res));
    if (::ioctl(m_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0 || res.count_crtcs == 0) return 0;
    std::vector<uint32_t> crtcs(res.count_crtcs);
    res.crtc_id_ptr = reinterpret_cast<uint64_t>(crtcs.data());
    res.count_connectors = res.count_encoders = res.count_fbs = 0;
    res.connector_id_ptr = res.encoder_id_ptr = res.fb_id_ptr = 0;
    if (::ioctl(m_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) return 0;
    // CRTCs that must not be taken: the playfield's, and panels already claimed.
    std::vector<uint32_t> taken;
    for (const Panel& p : m_panels) taken.push_back(p.crtcId);
    if (mainConnector) {
        drm_mode_get_connector mc;
        std::memset(&mc, 0, sizeof(mc));
        mc.connector_id = mainConnector;
        if (::ioctl(m_fd, DRM_IOCTL_MODE_GETCONNECTOR, &mc) == 0 && mc.encoder_id) {
            drm_mode_get_encoder me;
            std::memset(&me, 0, sizeof(me));
            me.encoder_id = mc.encoder_id;
            if (::ioctl(m_fd, DRM_IOCTL_MODE_GETENCODER, &me) == 0 && me.crtc_id) taken.push_back(me.crtc_id);
        }
    }
    for (uint32_t e = 0; e < count && e < encoders.size(); ++e) {
        drm_mode_get_encoder en;
        std::memset(&en, 0, sizeof(en));
        en.encoder_id = encoders[e];
        if (::ioctl(m_fd, DRM_IOCTL_MODE_GETENCODER, &en) != 0) continue;
        for (uint32_t i = 0; i < crtcs.size() && i < 32; ++i) {
            if (!(en.possible_crtcs & (1u << i))) continue;
            if (std::find(taken.begin(), taken.end(), crtcs[i]) != taken.end()) continue;
            log("panels: connector had no active encoder; using encoder %u with crtc %u", en.encoder_id, crtcs[i]);
            return crtcs[i];
        }
    }
    return 0;
}

int GamePanels::init(const DisplayProfile::Topology& topo, Mode mode) {
    m_mode = mode;
    m_fd = findCard0Fd();
    if (m_fd < 0) { log("panels: no card0 fd"); return 0; }
    const DisplayProfile::Screen* first = mode == Mode::Playing ? &topo.main : &topo.backglass;
    for (const DisplayProfile::Screen* scr : {first, &topo.dmd}) {
        if (!scr->available || scr->connectorId == 0) continue;
        Panel::Role role = scr == &topo.dmd ? Panel::Role::Dmd
                           : scr == &topo.main ? Panel::Role::Playfield : Panel::Role::Backglass;
        // While a game plays, SDL owns the backglass; the playfield is free.
        uint32_t avoid = mode == Mode::Playing ? topo.backglass.connectorId : topo.main.connectorId;
        drm_mode_get_connector cn;
        std::memset(&cn, 0, sizeof(cn));
        cn.connector_id = scr->connectorId;
        if (::ioctl(m_fd, DRM_IOCTL_MODE_GETCONNECTOR, &cn) != 0 || cn.count_modes == 0) {
            log("panels: connector %u unavailable", scr->connectorId);
            continue;
        }
        std::vector<drm_mode_modeinfo> modes(cn.count_modes);
        std::vector<uint32_t> encoders(cn.count_encoders ? cn.count_encoders : 1);
        cn.modes_ptr = reinterpret_cast<uint64_t>(modes.data());
        cn.encoders_ptr = reinterpret_cast<uint64_t>(encoders.data());
        cn.count_props = 0;
        cn.props_ptr = cn.prop_values_ptr = 0;
        ::ioctl(m_fd, DRM_IOCTL_MODE_GETCONNECTOR, &cn);
        uint32_t crtc = pickCrtc(cn.encoder_id, encoders, cn.count_encoders, avoid);
        if (!crtc) { log("panels: no display controller for connector %u", scr->connectorId); continue; }
        const drm_mode_modeinfo mode = modes[0];

        drm_mode_create_dumb cd;
        std::memset(&cd, 0, sizeof(cd));
        cd.width = mode.hdisplay; cd.height = mode.vdisplay; cd.bpp = 32;
        if (::ioctl(m_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) != 0) { log("panels: CREATE_DUMB %s", std::strerror(errno)); continue; }
        drm_mode_fb_cmd fbc;
        std::memset(&fbc, 0, sizeof(fbc));
        fbc.width = cd.width; fbc.height = cd.height; fbc.pitch = cd.pitch; fbc.bpp = 32; fbc.depth = 24; fbc.handle = cd.handle;
        if (::ioctl(m_fd, DRM_IOCTL_MODE_ADDFB, &fbc) != 0) { log("panels: ADDFB %s", std::strerror(errno)); continue; }
        drm_mode_map_dumb md;
        std::memset(&md, 0, sizeof(md));
        md.handle = cd.handle;
        if (::ioctl(m_fd, DRM_IOCTL_MODE_MAP_DUMB, &md) != 0) continue;
        uint8_t* map = (uint8_t*)::mmap(nullptr, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, md.offset);
        if (map == MAP_FAILED) continue;

        Panel p;
        p.connId = scr->connectorId; p.crtcId = crtc; p.fbId = fbc.fb_id; p.handle = cd.handle;
        p.pitch = cd.pitch; p.w = mode.hdisplay; p.h = mode.vdisplay; p.map = map; p.size = cd.size;
        p.role = role;
        if (role == Panel::Role::Playfield) {
            // Mounted portrait: compose portrait and turn it 90 degrees like the
            // menu's canvas (the same buffer mapping as the DMD's).
            p.rot = topo.main.rotationDegrees == 90 || topo.main.rotationDegrees == 270;
            p.vw = p.rot ? p.h : p.w;
            p.vh = p.rot ? p.w : p.h;
        } else {
            p.rot = p.h > p.w;
            p.vw = p.rot ? p.h : p.w;
            p.vh = p.rot ? p.w : p.h;
        }
        p.canvas.assign((size_t)p.vw * p.vh * 4, 0);

        drm_mode_crtc sc;
        std::memset(&sc, 0, sizeof(sc));
        sc.crtc_id = p.crtcId; sc.fb_id = p.fbId;
        sc.set_connectors_ptr = reinterpret_cast<uint64_t>(&p.connId);
        sc.count_connectors = 1; sc.mode = mode; sc.mode_valid = 1;
        if (::ioctl(m_fd, DRM_IOCTL_MODE_SETCRTC, &sc) != 0) {
            log("panels: SETCRTC %u: %s", p.connId, std::strerror(errno));
            ::munmap(map, cd.size);
            continue;
        }
        log("panels: %s up on connector %u (%dx%d%s)",
            role == Panel::Role::Dmd ? "DMD" : role == Panel::Role::Playfield ? "playfield" : "backglass", p.connId, p.w,
            p.h, p.rot ? ", rotated" : "");
        m_panels.push_back(std::move(p));
    }
    if (!m_panels.empty()) m_thread = std::thread(&GamePanels::worker, this);
    return (int)m_panels.size();
}

void GamePanels::show(const Item& item, float dt) {
    if (m_panels.empty()) return;
    if (item.key != m_pendingKey) { m_pendingKey = item.key; m_stable = 0.0f; }
    else m_stable += dt;
    // Wait until the highlight rests a moment (or immediately the first time),
    // then hand it to the worker; a newer item replaces one not yet started.
    if (m_pendingKey != m_shownKey && (m_stable >= 0.12f || m_shownKey == "\x01")) {
        m_shownKey = m_pendingKey;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_job = item;
            m_hasJob = true;
        }
        m_cv.notify_one();
    }
}

void GamePanels::worker() {
    for (;;) {
        Item job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stop || m_hasJob; });
            if (m_stop) return;
            job = m_job;
            m_hasJob = false;
        }
        auto t0 = std::chrono::steady_clock::now();
        compose(job);
        long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        log("panels: %s in %ld ms%s", job.key.c_str(), ms, job.artPath.empty() ? "" : " (with art)");
    }
}

void GamePanels::compose(const Item& item) {
    std::vector<uint8_t> art;
    int aw = 0, ah = 0;
    if (!item.artPath.empty()) {
        int n = 0;
        if (uint8_t* px = stbi_load(item.artPath.c_str(), &aw, &ah, &n, 4)) {
            art.assign(px, px + (size_t)aw * ah * 4);
            stbi_image_free(px);
        } else {
            log("panels: could not decode %s", item.artPath.c_str());
        }
    }
    // No art yet: a generated cover (games only; systems keep the title card).
    if (art.empty() && !item.title.empty()) art = makeCover(item.system, item.title, aw, ah);
    std::vector<uint8_t> console;
    int cw = 0, ch = 0;
    if (!item.consolePath.empty()) {
        if (loadImage(item.consolePath, console, cw, ch)) keyOutWhite(console, cw, ch);
        else log("panels: could not decode %s", item.consolePath.c_str());
    }
    for (Panel& p : m_panels) {
        if (p.role == Panel::Role::Dmd) composeDmd(p, item, console, cw, ch);
        else if (p.role == Panel::Role::Playfield) composePlayfield(p, item, art, aw, ah);
        else composeBackglass(p, item, art, aw, ah);
        present(p);
    }
}

void GamePanels::composeBackglass(Panel& p, const Item& item, const std::vector<uint8_t>& art, int aw, int ah) {
    const int W = p.vw, H = p.vh;
    std::vector<uint8_t>& c = p.canvas;
    const std::string headline = item.title.empty() ? item.system : item.title;
    if (!art.empty()) {
        backdrop(c, W, H, art, aw, ah);
        blitFit(c, W, H, art, aw, ah, (int)(W * 0.08f), (int)(H * 0.07f), (int)(W * 0.84f), (int)(H * 0.66f));
        text(c, W, H, headline, W * 0.5f, H * 0.78f, H * 0.085f, kInk, PanelFont::Face::Display, W * 0.9f);
        text(c, W, H, item.detail.empty() ? item.system : item.detail, W * 0.5f, H * 0.885f, H * 0.045f, kGold,
             PanelFont::Face::Body, W * 0.8f);
        return;
    }
    // Title card: Neon gradient, two light pools, big title.
    gradient(c, W, H, kGradTop, kGradBot);
    glow(c, W, H, W * 0.18f, H * 0.2f, H * 0.9f, kTeal, 0.22f);
    glow(c, W, H, W * 0.85f, H * 0.85f, H * 1.0f, kViolet, 0.25f);
    text(c, W, H, item.title.empty() ? "RETRO LAUNCHER" : item.system, W * 0.5f, H * 0.22f, H * 0.06f, kTeal,
         PanelFont::Face::Display, W * 0.8f);
    text(c, W, H, headline, W * 0.5f, H * 0.36f, H * 0.18f, kInk, PanelFont::Face::Display, W * 0.88f);
    text(c, W, H, item.detail, W * 0.5f, H * 0.62f, H * 0.055f, kGold, PanelFont::Face::Body, W * 0.8f);
}

void GamePanels::composeDmd(Panel& p, const Item& item, const std::vector<uint8_t>& console, int cw, int ch) {
    const int W = p.vw, H = p.vh;
    std::vector<uint8_t>& c = p.canvas;
    gradient(c, W, H, kGradTop, kGradBot);
    glow(c, W, H, W * 0.5f, H * 0.5f, W * 0.5f, kTeal, 0.18f);
    if (m_mode == Mode::Playing) {
        // The playfield card already names the game: show the console itself.
        if (!console.empty()) {
            glow(c, W, H, W * 0.5f, H * 0.46f, W * 0.42f, kWhite, 0.10f);
            blitFit(c, W, H, console, cw, ch, (int)(W * 0.06f), (int)(H * 0.06f), (int)(W * 0.88f), (int)(H * 0.76f));
            text(c, W, H, item.system, W * 0.5f, H * 0.85f, H * 0.09f, kGold, PanelFont::Face::Display, W * 0.9f);
        } else {
            text(c, W, H, item.system, W * 0.5f, H * 0.32f, H * 0.32f, kInk, PanelFont::Face::Display, W * 0.92f);
        }
        return;
    }
    if (item.title.empty()) {
        text(c, W, H, "RETRO LAUNCHER", W * 0.5f, H * 0.16f, H * 0.12f, kTeal, PanelFont::Face::Display, W * 0.9f);
        text(c, W, H, item.system, W * 0.5f, H * 0.38f, H * 0.26f, kInk, PanelFont::Face::Display, W * 0.92f);
    } else {
        text(c, W, H, item.system, W * 0.5f, H * 0.14f, H * 0.13f, kGold, PanelFont::Face::Display, W * 0.9f);
        text(c, W, H, item.title, W * 0.5f, H * 0.38f, H * 0.24f, kInk, PanelFont::Face::Display, W * 0.94f);
    }
    text(c, W, H, item.detail, W * 0.5f, H * 0.76f, H * 0.09f, kTeal, PanelFont::Face::Body, W * 0.9f);
}

// Darken a rectangle (a translucent card) with a light top edge.
void card(std::vector<uint8_t>& c, int W, int H, int x0, int y0, int w, int h, float darken) {
    for (int y = std::max(0, y0); y < std::min(H, y0 + h); ++y)
        for (int x = std::max(0, x0); x < std::min(W, x0 + w); ++x) {
            uint8_t* p = &c[((size_t)y * W + x) * 4];
            bool edge = y < y0 + 3;
            for (int k = 0; k < 3; ++k) p[k] = edge ? (uint8_t)std::min(255, p[k] + 60) : (uint8_t)(p[k] * darken);
        }
}

// Playing mode, playfield (portrait): cover, title, controls, how to exit.
void GamePanels::composePlayfield(Panel& p, const Item& item, const std::vector<uint8_t>& art, int aw, int ah) {
    const int W = p.vw, H = p.vh;
    const float u = W / 720.0f;  // design units: the menu's 720-wide portrait canvas
    std::vector<uint8_t>& c = p.canvas;
    if (!art.empty()) backdrop(c, W, H, art, aw, ah);
    else gradient(c, W, H, kGradTop, kGradBot);
    glow(c, W, H, W * 0.15f, H * 0.08f, W * 0.9f, kTeal, 0.10f);
    glow(c, W, H, W * 0.9f, H * 0.95f, W * 1.0f, kViolet, 0.14f);

    text(c, W, H, "NOW PLAYING", W * 0.5f, 64 * u, 30 * u, kTeal, PanelFont::Face::Display, W * 0.8f);
    if (!art.empty())
        blitFit(c, W, H, art, aw, ah, (int)(130 * u), (int)(120 * u), (int)(460 * u), (int)(560 * u));
    text(c, W, H, item.title, W * 0.5f, 712 * u, 64 * u, kInk, PanelFont::Face::Display, W * 0.88f);
    std::string sub = item.detail.empty() ? item.system : item.system + "   " + item.detail;
    text(c, W, H, sub, W * 0.5f, 790 * u, 26 * u, kGold, PanelFont::Face::Body, W * 0.86f);

    // Controls card.
    if (!item.controls.empty()) {
        int rows = (int)item.controls.size();
        int cardY = (int)(850 * u), rowH = (int)(46 * u), cardH = (int)(64 * u) + rows * rowH;
        card(c, W, H, (int)(48 * u), cardY, W - (int)(96 * u), cardH, 0.45f);
        text(c, W, H, "CONTROLS", W * 0.5f, cardY + 16 * u, 22 * u, kTeal, PanelFont::Face::Display, W * 0.6f);
        for (int i = 0; i < rows; ++i) {
            float y = cardY + 56 * u + i * rowH;
            const std::string& key = item.controls[i].first;
            float kw = PanelFont::measure(key, 30 * u, PanelFont::Face::Display);
            PanelFont::draw(c.data(), W, H, key, W * 0.40f - kw, y, 30 * u, kGold[0], kGold[1], kGold[2],
                            PanelFont::Face::Display);
            PanelFont::draw(c.data(), W, H, item.controls[i].second, W * 0.46f, y + 2 * u, 28 * u, kInk[0], kInk[1],
                            kInk[2], PanelFont::Face::Body);
        }
    }
    text(c, W, H, "Hold START or press HOME for the pause menu", W * 0.5f, H - 74 * u, 24 * u, kInk,
         PanelFont::Face::Body, W * 0.9f);
}

void GamePanels::present(Panel& p) {
    const uint8_t* c = p.canvas.data();
    for (int vy = 0; vy < p.vh; ++vy) {
        const uint8_t* srow = c + (size_t)vy * p.vw * 4;
        if (!p.rot) {
            uint32_t* dst = reinterpret_cast<uint32_t*>(p.map + (size_t)vy * p.pitch);
            for (int vx = 0; vx < p.vw; ++vx) {
                const uint8_t* s = srow + vx * 4;
                dst[vx] = (uint32_t(s[0]) << 16) | (uint32_t(s[1]) << 8) | s[2];
            }
        } else {
            for (int vx = 0; vx < p.vw; ++vx) {  // 90° CCW for the portrait-native DMD
                const uint8_t* s = srow + vx * 4;
                int bx = p.w - 1 - vy, by = vx;
                *reinterpret_cast<uint32_t*>(p.map + (size_t)by * p.pitch + bx * 4) =
                    (uint32_t(s[0]) << 16) | (uint32_t(s[1]) << 8) | s[2];
            }
        }
    }
}
