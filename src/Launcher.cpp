#include "Launcher.h"

#include "AppConfig.h"
#include "AppFont.h"
#include "DisplayProfile.h"
#include "GamePanels.h"
#include "Gfx.h"
#include "Library.h"
#include "Theme.h"
#include "Version.h"
#include "controls/Controls.h"

#include <SDL.h>

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

// The menu: Systems -> Games -> play, plus Search across every system. Neon
// theme on a portrait canvas on the playfield (the Jukebox window setup); the
// backglass and DMD show the highlighted game (GamePanels). Launching a game
// execs this binary in play mode, so the menu process ends there.

namespace {

using Library::log;
using Library::ScreenId;

constexpr float kListTop = 210.0f;
constexpr float kRowH = 92.0f;
constexpr float kRowGap = 10.0f;
constexpr float kListBottom = Theme::kFooterTop - 24.0f;

// Search layout.
constexpr float kQueryTop = 206.0f;
constexpr float kKeysTop = 300.0f;
constexpr float kKeyH = 54.0f;
constexpr float kKeyGap = 8.0f;
constexpr float kHitsTop = 640.0f;
constexpr float kHitH = 72.0f;
constexpr float kHitGap = 8.0f;

struct SystemEntry {
    const Library::System* sys;
    std::vector<Library::Game> games;
};

struct Key { std::string label; char ch; float units; };
enum : char { kSpace = ' ', kDelete = '\b', kResults = '\n' };

const std::vector<std::vector<Key>>& keyboard() {
    static const std::vector<std::vector<Key>> rows = [] {
        std::vector<std::vector<Key>> r;
        for (const char* line : {"ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ1234", "567890-&'."}) {
            std::vector<Key> row;
            for (const char* c = line; *c; ++c) row.push_back({std::string(1, *c), *c, 1.0f});
            r.push_back(row);
        }
        r.push_back({{"SPACE", kSpace, 4.0f}, {"DEL", kDelete, 3.0f}, {"RESULTS", kResults, 3.0f}});
        return r;
    }();
    return rows;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Alphabet group for letter jumps: 'A'..'Z', or '#' for anything else.
char groupOf(const std::string& title) {
    for (char c : title) {
        if (std::isalpha((unsigned char)c)) return (char)std::toupper((unsigned char)c);
        if (std::isdigit((unsigned char)c)) return '#';
    }
    return '#';
}

class Menu {
public:
    Menu(std::string appDir, std::string startSys, int startIndex, std::string message)
        : m_appDir(std::move(appDir)), m_startSys(std::move(startSys)), m_startIndex(startIndex),
          m_toast(std::move(message)) {}

    int run();

private:
    enum class View { Systems, Games, Search, Recent };

    bool initVideo();
    void shutdown();
    void scan();
    void handle(AtGames::ControlEvent ev, bool& running);
    void handleSearch(AtGames::ControlEvent ev);
    void render(float dt);
    void renderSystems();
    void renderGames();
    void renderSearch();
    void renderRecent();
    void loadRecent();
    void renderToast();
    void renderJump();
    void drawHeader(const std::string& caption, const std::string& title, int index, int total);
    void beginListClip(float top, float bottom);
    void endListClip();
    void present();
    void move(int delta);
    void jumpLetter(int dir);
    void openSearch();
    void runQuery();
    void pressKey();
    void launch(int sys, int game, const std::string& returnTo = "", int returnIndex = -1);
    void updatePanels(float dt);

    std::string m_appDir, m_startSys;
    int m_startIndex = 0;
    std::string m_toast;
    float m_toastTime = 0.0f;

    DisplayProfile::Topology m_topo;
    Library::Settings m_settings;
    std::vector<SystemEntry> m_systems;
    View m_view = View::Systems;
    int m_sysRow = 0;   // 0 = Search, 1 = Recently played, 2.. = m_systems[row - 2]
    int m_gameSel = 0;
    float m_sysScroll = 0.0f, m_gameScroll = 0.0f;
    float m_clock = 0.0f;
    char m_jumpChar = 0;
    float m_jumpTime = 0.0f;

    // Search state.
    struct Hit { int sys, game; };
    std::string m_query;
    std::vector<Hit> m_hits;
    int m_kbRow = 0, m_kbCol = 0;
    bool m_inHits = false;
    int m_hitSel = 0;
    float m_hitScroll = 0.0f;
    View m_searchReturn = View::Systems;

    // Recently played, resolved against the scanned games (missing ones skipped).
    std::vector<Hit> m_recent;
    int m_recentSel = 0;
    float m_recentScroll = 0.0f;

    // Backglass / DMD.
    std::unique_ptr<GamePanels> m_panels;
    std::string m_panelKey;
    GamePanels::Item m_panelItem;

    SDL_Window* m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture* m_canvas = nullptr;
    float m_canvasScale = 1.0f;
    int m_canvasW = 0, m_canvasH = 0;
    AtGames::Controls m_controls;

    int sysIndex() const { return m_sysRow - 2; }
    static constexpr int kFixedRows = 2;  // Search, Recently played
};

// ----------------------------------------------------------------- setup

bool Menu::initVideo() {
    m_topo = DisplayProfile::detect();
    log("model '%s' %s: main %u, backglass %u, dmd %u", m_topo.model.c_str(), m_topo.known ? "known" : "unknown",
        m_topo.main.connectorId, m_topo.backglass.connectorId, m_topo.dmd.connectorId);
    // The menu lives on the playfield. Set (or clear) both variables explicitly:
    // the environment carries over from the game process.
    ::unsetenv("SDL2_DISPLAY_PLANE_TYPE");
    ::unsetenv("ForceConnectID");
    DisplayProfile::prepareSdlMain(m_topo);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0) {
        log("SDL_Init: %s", SDL_GetError());
        return false;
    }
    m_window = SDL_CreateWindow("Retro Launcher", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                AppConfig::kFramebufferWidth, AppConfig::kFramebufferHeight,
                                SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!m_window) { log("window: %s", SDL_GetError()); return false; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    m_renderer = SDL_CreateRenderer(m_window, -1,
                                    SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
    if (!m_renderer) m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_TARGETTEXTURE);
    if (!m_renderer) { log("renderer: %s", SDL_GetError()); return false; }

    // Portrait canvas, rotated onto the landscape framebuffer (as Jukebox does).
    int ow = AppConfig::kFramebufferWidth, oh = AppConfig::kFramebufferHeight;
    SDL_GetRendererOutputSize(m_renderer, &ow, &oh);
    m_canvasScale = std::max(1.0f, std::min(ow / (float)AppConfig::kLogicalHeight, oh / (float)AppConfig::kLogicalWidth));
    m_canvasW = (int)std::lround(AppConfig::kLogicalWidth * m_canvasScale);
    m_canvasH = (int)std::lround(AppConfig::kLogicalHeight * m_canvasScale);
    m_canvas = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, m_canvasW, m_canvasH);
    if (!m_canvas) {
        m_canvasScale = 1.0f;
        m_canvasW = AppConfig::kLogicalWidth;
        m_canvasH = AppConfig::kLogicalHeight;
        m_canvas = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, m_canvasW, m_canvasH);
        if (!m_canvas) { log("canvas: %s", SDL_GetError()); return false; }
    }
    SDL_SetTextureBlendMode(m_canvas, SDL_BLENDMODE_NONE);
    log("menu output %dx%d, canvas scale %.2f", ow, oh, m_canvasScale);
    Gfx::init(m_renderer, m_canvasScale);
    m_controls.open();

    // Secondary panels reuse SDL's card0 fd, so they come up after SDL.
    // "panels = off" in data/settings.cfg leaves the backglass/DMD alone.
    if (m_settings.value("panels", "on") != "off") {
        m_panels.reset(new GamePanels());
        log("panels: %d up", m_panels->init(m_topo));
    } else {
        log("panels: off by setting");
    }
    m_panelKey.clear();
    return true;
}

void Menu::shutdown() {
    m_panels.reset();  // releases its buffers on SDL's fd before SDL closes it
    m_controls.close();
    Gfx::shutdown();
    if (m_canvas) SDL_DestroyTexture(m_canvas);
    if (m_renderer) SDL_DestroyRenderer(m_renderer);
    if (m_window) SDL_DestroyWindow(m_window);
    m_canvas = nullptr; m_renderer = nullptr; m_window = nullptr;
    SDL_Quit();
}

void Menu::scan() {
    m_systems.clear();
    for (const Library::System& s : Library::systems()) {
        SystemEntry e{&s, Library::scanGames(m_appDir, s)};
        log("%s: %zu game(s)", s.id.c_str(), e.games.size());
        m_systems.push_back(std::move(e));
    }
}

// ----------------------------------------------------------------- input

void Menu::move(int delta) {
    if (m_view == View::Systems) {
        int n = (int)m_systems.size() + kFixedRows;
        m_sysRow = (m_sysRow + delta % n + n) % n;
        return;
    }
    if (m_view == View::Recent) {
        int n = (int)m_recent.size();
        if (n) m_recentSel = (m_recentSel + delta % n + n) % n;
        return;
    }
    int n = (int)m_systems[sysIndex()].games.size();
    if (n == 0) return;
    m_gameSel = (m_gameSel + delta + n) % n;
}

// Flippers in a game list: jump to the start of the previous / next letter.
void Menu::jumpLetter(int dir) {
    const std::vector<Library::Game>& games = m_systems[sysIndex()].games;
    int n = (int)games.size();
    if (n == 0) return;
    char cur = groupOf(games[m_gameSel].title);
    int i = m_gameSel;
    if (dir > 0) {
        while (i < n && groupOf(games[i].title) == cur) ++i;
        if (i >= n) i = 0;  // wrap to the top
    } else {
        int first = i;
        while (first > 0 && groupOf(games[first - 1].title) == cur) --first;
        if (first != m_gameSel) {
            i = first;
        } else {
            i = first == 0 ? n - 1 : first - 1;  // previous group (wrapping)
            char prev = groupOf(games[i].title);
            while (i > 0 && groupOf(games[i - 1].title) == prev) --i;
        }
    }
    m_gameSel = i;
    m_jumpChar = groupOf(games[i].title);
    m_jumpTime = 0.0f;
}

void Menu::loadRecent() {
    m_recent.clear();
    for (auto& entry : Library::loadRecent(m_appDir))
        for (int s = 0; s < (int)m_systems.size(); ++s) {
            if (m_systems[s].sys->id != entry.first) continue;
            for (int g = 0; g < (int)m_systems[s].games.size(); ++g)
                if (m_systems[s].games[g].file == entry.second) { m_recent.push_back({s, g}); break; }
        }
}

void Menu::launch(int sys, int game, const std::string& returnTo, int returnIndex) {
    SystemEntry& e = m_systems[sys];
    if (game < 0 || game >= (int)e.games.size()) return;
    const Library::Game& g = e.games[game];
    Library::pushRecent(m_appDir, e.sys->id, g.file);
    // Games play on the backglass for now; per-game screen choice comes later
    // (Settings and the player already support it).
    const ScreenId screen = ScreenId::Backglass;
    // Draw a "Starting" frame so the press registers before the screen goes dark.
    m_toast = "Starting " + g.title + "...";
    m_toastTime = 0.0f;
    render(0.0f);
    present();
    shutdown();
    Library::execPlay(m_appDir, e.sys->id, g.path, screen, returnIndex >= 0 ? returnIndex : game, returnTo);
    // Only reached if exec failed: come back up.
    initVideo();
    m_toast = "Could not start the game (see data/launcher.log).";
}

void Menu::openSearch() {
    m_searchReturn = m_view == View::Search ? View::Systems : m_view;
    m_view = View::Search;
    m_inHits = false;
    m_kbRow = m_kbCol = 0;
    runQuery();
}

// Every word of the query must appear in the title or tags (case-insensitive).
void Menu::runQuery() {
    m_hits.clear();
    m_hitSel = 0;
    m_hitScroll = 0.0f;
    std::vector<std::string> words;
    std::string q = lower(m_query), w;
    for (char c : q + " ") {
        if (c == ' ') { if (!w.empty()) words.push_back(w); w.clear(); }
        else w += c;
    }
    if (words.empty()) return;
    for (int s = 0; s < (int)m_systems.size(); ++s)
        for (int g = 0; g < (int)m_systems[s].games.size(); ++g) {
            const Library::Game& game = m_systems[s].games[g];
            std::string hay = lower(game.title + " " + game.tags);
            bool all = true;
            for (const std::string& word : words) all = all && hay.find(word) != std::string::npos;
            if (all) m_hits.push_back({s, g});
            if (m_hits.size() >= 500) goto sort;  // cap, but still sort what we have
        }
sort:
    // Titles that START with the query first, then the rest, each alphabetical.
    std::stable_sort(m_hits.begin(), m_hits.end(), [&](const Hit& a, const Hit& b) {
        bool sa = lower(m_systems[a.sys].games[a.game].title).compare(0, words[0].size(), words[0]) == 0;
        bool sb = lower(m_systems[b.sys].games[b.game].title).compare(0, words[0].size(), words[0]) == 0;
        if (sa != sb) return sa;
        return lower(m_systems[a.sys].games[a.game].title) < lower(m_systems[b.sys].games[b.game].title);
    });
}

void Menu::pressKey() {
    const std::vector<std::vector<Key>>& kb = keyboard();
    const Key& k = kb[m_kbRow][std::min(m_kbCol, (int)kb[m_kbRow].size() - 1)];
    if (k.ch == kDelete) { if (!m_query.empty()) m_query.pop_back(); }
    else if (k.ch == kResults) { if (!m_hits.empty()) m_inHits = true; return; }
    else if (k.ch == kSpace) { if (!m_query.empty() && m_query.back() != ' ') m_query += ' '; }
    else if (m_query.size() < 32) m_query += k.ch;
    runQuery();
}

void Menu::handleSearch(AtGames::ControlEvent ev) {
    using CE = AtGames::ControlEvent;
    const std::vector<std::vector<Key>>& kb = keyboard();
    auto back = [&]() {
        if (m_inHits) { m_inHits = false; return; }
        if (!m_query.empty()) { m_query.pop_back(); runQuery(); return; }
        m_view = m_searchReturn;
    };
    if (ev == CE::B || ev == CE::Back || ev == CE::Rewind || ev == CE::Rewind2) { back(); return; }
    if (ev == CE::LeftShoulder) { if (!m_query.empty()) { m_query.pop_back(); runQuery(); } return; }
    if (ev == CE::RightShoulder) { if (!m_hits.empty()) m_inHits = true; return; }
    if (m_inHits) {
        int n = (int)m_hits.size();
        if (ev == CE::Up) { if (m_hitSel == 0) m_inHits = false; else --m_hitSel; }
        else if (ev == CE::Down) m_hitSel = std::min(n - 1, m_hitSel + 1);
        else if (ev == CE::A || ev == CE::Start) launch(m_hits[m_hitSel].sys, m_hits[m_hitSel].game);
        return;
    }
    // Keyboard. Columns map proportionally between rows of different widths.
    auto colX = [&](int row, int col) {
        float u = 0, total = 0;
        for (const Key& k : kb[row]) total += k.units;
        for (int i = 0; i < col; ++i) u += kb[row][i].units;
        return (u + kb[row][col].units * 0.5f) / total;
    };
    auto colAt = [&](int row, float x) {
        float total = 0, u = 0;
        for (const Key& k : kb[row]) total += k.units;
        for (int i = 0; i < (int)kb[row].size(); ++i) {
            u += kb[row][i].units;
            if (x * total <= u) return i;
        }
        return (int)kb[row].size() - 1;
    };
    int rows = (int)kb.size();
    if (ev == CE::Left) m_kbCol = (m_kbCol + (int)kb[m_kbRow].size() - 1) % (int)kb[m_kbRow].size();
    else if (ev == CE::Right) m_kbCol = (m_kbCol + 1) % (int)kb[m_kbRow].size();
    else if (ev == CE::Up && m_kbRow > 0) { float x = colX(m_kbRow, m_kbCol); --m_kbRow; m_kbCol = colAt(m_kbRow, x); }
    else if (ev == CE::Down) {
        if (m_kbRow < rows - 1) { float x = colX(m_kbRow, m_kbCol); ++m_kbRow; m_kbCol = colAt(m_kbRow, x); }
        else if (!m_hits.empty()) m_inHits = true;
    } else if (ev == CE::A || ev == CE::Start) pressKey();
}

void Menu::handle(AtGames::ControlEvent ev, bool& running) {
    using CE = AtGames::ControlEvent;
    if (m_view == View::Search) { handleSearch(ev); return; }
    switch (ev) {
    case CE::Up: move(-1); break;
    case CE::Down: move(1); break;
    case CE::LeftShoulder:
        if (m_view == View::Games) jumpLetter(-1); else move(-4);
        break;
    case CE::RightShoulder:
        if (m_view == View::Games) jumpLetter(1); else move(4);
        break;
    case CE::X:
    case CE::Y:
        openSearch();
        break;
    case CE::A:
    case CE::Start:
        if (m_view == View::Systems) {
            if (m_sysRow == 0) { openSearch(); break; }
            if (m_sysRow == 1) {
                loadRecent();
                if (m_recent.empty()) {
                    m_toast = "Nothing played yet - games you start will show up here";
                    m_toastTime = 0.0f;
                } else {
                    m_view = View::Recent;
                    m_recentSel = 0;
                    m_recentScroll = 0.0f;
                }
                break;
            }
            SystemEntry& e = m_systems[sysIndex()];
            if (e.games.empty()) {
                m_toast = "Add " + e.sys->name + " ROMs to roms/" + e.sys->id + "/";
                m_toastTime = 0.0f;
            } else {
                m_view = View::Games;
                m_gameSel = 0;
                m_gameScroll = 0.0f;
            }
        } else if (m_view == View::Recent) {
            // The game moves to the top of the list, so come back to row 0.
            if (!m_recent.empty()) launch(m_recent[m_recentSel].sys, m_recent[m_recentSel].game, "@recent", 0);
        } else {
            launch(sysIndex(), m_gameSel);
        }
        break;
    case CE::B:
    case CE::Back:
    case CE::Rewind:
    case CE::Rewind2:
        if (m_view == View::Games || m_view == View::Recent) m_view = View::Systems;
        else running = false;
        break;
    default: break;
    }
}

// ----------------------------------------------------------------- panels

void Menu::updatePanels(float dt) {
    if (!m_panels || !m_panels->active()) return;
    std::string key;
    int sys = -1, game = -1;
    if (m_view == View::Games) { sys = sysIndex(); game = m_gameSel; }
    else if (m_view == View::Search && m_inHits && !m_hits.empty()) { sys = m_hits[m_hitSel].sys; game = m_hits[m_hitSel].game; }
    else if (m_view == View::Recent && !m_recent.empty()) { sys = m_recent[m_recentSel].sys; game = m_recent[m_recentSel].game; }

    if (sys >= 0 && game >= 0 && game < (int)m_systems[sys].games.size()) {
        const Library::Game& g = m_systems[sys].games[game];
        key = "game:" + m_systems[sys].sys->id + "/" + g.file;
        if (key != m_panelKey) {
            m_panelItem = {key, m_systems[sys].sys->name, g.title, g.tags,
                           findArt(m_appDir, m_systems[sys].sys->id, g.file)};
        }
    } else if (m_view == View::Search) {
        key = "search:" + std::to_string(m_hits.size());
        if (key != m_panelKey)
            m_panelItem = {key, "Search", "", m_hits.empty() ? "Type to find a game" :
                           std::to_string(m_hits.size()) + (m_hits.size() == 1 ? " match" : " matches"), ""};
    } else if (m_sysRow == 0) {
        key = "row:search";
        if (key != m_panelKey) m_panelItem = {key, "Search", "", "Find any game", ""};
    } else if (m_sysRow == 1) {
        key = "row:recent";
        if (key != m_panelKey) m_panelItem = {key, "Recently played", "", "Your last games", ""};
    } else {
        const SystemEntry& e = m_systems[sysIndex()];
        key = "sys:" + e.sys->id;
        if (key != m_panelKey)
            m_panelItem = {key, e.sys->name, "",
                           e.games.empty() ? "No games yet" : std::to_string(e.games.size()) + " games", ""};
    }
    m_panelKey = key;
    m_panels->show(m_panelItem, dt);
}

// ----------------------------------------------------------------- drawing

// Keeps the selected row inside the visible list, easing the scroll.
float scrollFor(float current, int sel, int count, float dt, float top = kListTop, float bottom = kListBottom,
                float step = kRowH + kRowGap) {
    const float viewH = bottom - top;
    float total = count * step;
    float wantTop = sel * step - (viewH - step) * 0.5f;
    wantTop = std::max(0.0f, std::min(wantTop, std::max(0.0f, total - viewH)));
    if (dt <= 0.0f) return wantTop;
    return current + (wantTop - current) * std::min(1.0f, dt * 14.0f);
}

void Menu::beginListClip(float top, float bottom) {
    SDL_Rect clip{0, (int)(top * m_canvasScale), (int)(AppConfig::kLogicalWidth * m_canvasScale),
                  (int)((bottom - top) * m_canvasScale)};
    SDL_RenderSetScale(m_renderer, 1.0f, 1.0f);
    SDL_RenderSetClipRect(m_renderer, &clip);
    SDL_RenderSetScale(m_renderer, m_canvasScale, m_canvasScale);
}

void Menu::endListClip() {
    SDL_RenderSetScale(m_renderer, 1.0f, 1.0f);
    SDL_RenderSetClipRect(m_renderer, nullptr);
    SDL_RenderSetScale(m_renderer, m_canvasScale, m_canvasScale);
}

void Menu::renderSystems() {
    SDL_Renderer* r = m_renderer;
    const int w = AppConfig::kLogicalWidth;
    beginListClip(kListTop, kListBottom);
    for (int i = 0; i < (int)m_systems.size() + kFixedRows; ++i) {
        float y = kListTop + i * (kRowH + kRowGap) - m_sysScroll;
        if (y + kRowH < kListTop || y > kListBottom) continue;
        bool active = i == m_sysRow;
        FRect row{Theme::kMargin - 16.0f, y, w - 2.0f * (Theme::kMargin - 16.0f), kRowH};
        Theme::rowCard(r, row, active);
        float tx = row.x + 100.0f;
        if (i == 0) {
            Theme::monogram(r, row.x + 52.0f, y + kRowH * 0.5f, 28.0f, "?", Theme::Accent);
            AppFont::draw(r, "Search", tx, y + 14.0f, Theme::Type::Body, active ? Theme::Text : Theme::TextDim);
            AppFont::draw(r, "Find any game on any system", tx, y + 54.0f, Theme::Type::Caption, Theme::Muted);
            continue;
        }
        if (i == 1) {
            Theme::monogram(r, row.x + 52.0f, y + kRowH * 0.5f, 28.0f, "R", Theme::Accent2);
            AppFont::draw(r, "Recently played", tx, y + 14.0f, Theme::Type::Body, active ? Theme::Text : Theme::TextDim);
            AppFont::draw(r, "Your last 20 games", tx, y + 54.0f, Theme::Type::Caption, Theme::Muted);
            continue;
        }
        const SystemEntry& e = m_systems[i - kFixedRows];
        bool empty = e.games.empty();
        Theme::monogram(r, row.x + 52.0f, y + kRowH * 0.5f, 28.0f, e.sys->shortName,
                        empty ? SDL_Color{90, 98, 120, 255} : Theme::badgeColor(e.sys->id));
        SDL_Color tc = empty ? Theme::Faint : (active ? Theme::Text : Theme::TextDim);
        AppFont::draw(r, e.sys->name, tx, y + 14.0f, Theme::Type::Body, tc);
        std::string sub = empty ? "No games - add ROMs to roms/" + e.sys->id + "/"
                                : std::to_string(e.games.size()) + (e.games.size() == 1 ? " game" : " games");
        if (!e.sys->verified) sub += "   untested";
        AppFont::draw(r, sub, tx, y + 54.0f, Theme::Type::Caption, empty ? Theme::Faint : Theme::Muted);
    }
    endListClip();
    drawHeader("Retro Launcher", "Consoles", m_sysRow + 1, (int)m_systems.size() + kFixedRows);
    Theme::footerHints(r, w, "A Open   B Exit", "");
}

void Menu::renderGames() {
    SDL_Renderer* r = m_renderer;
    const int w = AppConfig::kLogicalWidth;
    const SystemEntry& e = m_systems[sysIndex()];
    beginListClip(kListTop, kListBottom);
    for (int i = 0; i < (int)e.games.size(); ++i) {
        float y = kListTop + i * (kRowH + kRowGap) - m_gameScroll;
        if (y + kRowH < kListTop || y > kListBottom) continue;
        const Library::Game& g = e.games[i];
        bool active = i == m_gameSel;
        FRect row{Theme::kMargin - 16.0f, y, w - 2.0f * (Theme::kMargin - 16.0f), kRowH};
        Theme::rowCard(r, row, active);
        float tx = row.x + 24.0f, availW = row.w - 48.0f;
        Theme::rowTitle(r, g.title, tx, y + 12.0f, availW, Theme::Type::Body, active ? Theme::Text : Theme::TextDim, active);
        if (!g.tags.empty())
            AppFont::draw(r, Theme::ellipsize(r, g.tags, availW, Theme::Type::Caption, AppFont::Face::Body), tx, y + 54.0f,
                          Theme::Type::Caption, Theme::Muted);
    }
    endListClip();
    drawHeader(e.sys->verified ? "Games" : "Games - untested core", e.sys->name, m_gameSel + 1, (int)e.games.size());
    Theme::footerHints(r, w, "A Play   LB/RB Letter   X Search   B Back", "");
}

void Menu::renderSearch() {
    SDL_Renderer* r = m_renderer;
    const int w = AppConfig::kLogicalWidth;
    const float x0 = Theme::kMargin - 16.0f, fullW = w - 2.0f * x0;

    // Results first (their marquee rows reset the clip), then everything above.
    beginListClip(kHitsTop, kListBottom);
    for (int i = 0; i < (int)m_hits.size(); ++i) {
        float y = kHitsTop + i * (kHitH + kHitGap) - m_hitScroll;
        if (y + kHitH < kHitsTop || y > kListBottom) continue;
        const SystemEntry& se = m_systems[m_hits[i].sys];
        const Library::Game& g = se.games[m_hits[i].game];
        bool active = m_inHits && i == m_hitSel;
        FRect row{x0, y, fullW, kHitH};
        Theme::rowCard(r, row, active);
        float chipW = Theme::chipWidth(r, se.sys->shortName, 32.0f);
        Theme::chip(r, row.x + row.w - 18.0f - chipW, y + (kHitH - 32.0f) * 0.5f, se.sys->shortName,
                    Theme::alpha(Theme::badgeColor(se.sys->id), 200), {255, 255, 255, 255}, 32.0f);
        std::string label = g.tags.empty() ? g.title : g.title + "  " + g.tags;
        Theme::rowTitle(r, label, row.x + 22.0f, y + 16.0f, row.w - 60.0f - chipW, Theme::Type::Small,
                        active ? Theme::Text : Theme::TextDim, active, 40.0f);
    }
    endListClip();
    // Results scroll under the keyboard area; repaint it before drawing on top.
    beginListClip(0.0f, kHitsTop - 2.0f);
    Theme::backgroundAnimated(r, w, AppConfig::kLogicalHeight, m_clock);
    endListClip();

    // Query box with a blinking caret.
    FRect box{x0, kQueryTop, fullW, 70.0f};
    Gfx::panel(r, box, 20.0f, {255, 255, 255, 20}, {255, 255, 255, 10},
               Theme::alpha(Theme::accent(), m_inHits ? 90 : 220), 2.0f);
    std::string shown = m_query.empty() ? "Type a game name" : m_query;
    AppFont::draw(r, Theme::ellipsize(r, shown, box.w - 60.0f, Theme::Type::Body, AppFont::Face::Body), box.x + 24.0f,
                  box.y + 18.0f, Theme::Type::Body, m_query.empty() ? Theme::Faint : Theme::Text);
    if (!m_inHits && std::fmod(m_clock, 1.0f) < 0.6f) {
        float cx = box.x + 24.0f + (m_query.empty() ? 0.0f : AppFont::measureWidth(r, m_query, Theme::Type::Body));
        Gfx::rect(r, {cx + 2.0f, box.y + 18.0f, 3.0f, 34.0f}, Theme::accent());
    }

    // Keyboard.
    const std::vector<std::vector<Key>>& kb = keyboard();
    for (int row = 0; row < (int)kb.size(); ++row) {
        float total = 0;
        for (const Key& k : kb[row]) total += k.units;
        float unitW = (fullW - kKeyGap * (kb[row].size() - 1)) / total;
        float x = x0, y = kKeysTop + row * (kKeyH + kKeyGap);
        for (int col = 0; col < (int)kb[row].size(); ++col) {
            const Key& k = kb[row][col];
            float kw = unitW * k.units + kKeyGap * (k.units - 1.0f);
            bool sel = !m_inHits && row == m_kbRow && col == m_kbCol;
            FRect kr{x, y, kw, kKeyH};
            if (sel) {
                Gfx::softRect(r, kr, 14.0f, 12.0f, Theme::alpha(Theme::accent(), 70), true);
                Gfx::roundRect(r, kr, 14.0f, Theme::accent());
            } else {
                Gfx::roundRect(r, kr, 14.0f, {255, 255, 255, 16});
            }
            bool word = k.label.size() > 1;
            float size = word ? Theme::Type::Caption : 30.0f;
            AppFont::drawCentered(r, k.label, x + kw * 0.5f, y + (kKeyH - size) * 0.5f - 3.0f, size,
                                  sel ? Theme::onAccent(Theme::accent()) : Theme::Text,
                                  word ? AppFont::Face::Body : AppFont::Face::Display);
            x += kw + kKeyGap;
        }
    }

    // Results heading (match count).
    Theme::tracked(r, m_query.empty() ? "RESULTS" : (std::to_string(m_hits.size()) + (m_hits.size() >= 500 ? "+" : "") +
                                                     (m_hits.size() == 1 ? " MATCH" : " MATCHES")),
                   Theme::kMargin, kHitsTop - 34.0f, Theme::Type::Caption, Theme::accent());
    if (m_hits.empty() && !m_query.empty())
        AppFont::drawCentered(r, "No games match", w * 0.5f, kHitsTop + 30.0f, Theme::Type::Small, Theme::Muted);

    drawHeader("Search", "All systems", 0, 0);
    Theme::footerHints(r, w, m_inHits ? "A Play   UP/DOWN Choose   B Keyboard" : "A Type   LB Delete   RB Results   B Back", "");
}

void Menu::renderRecent() {
    SDL_Renderer* r = m_renderer;
    const int w = AppConfig::kLogicalWidth;
    beginListClip(kListTop, kListBottom);
    for (int i = 0; i < (int)m_recent.size(); ++i) {
        float y = kListTop + i * (kRowH + kRowGap) - m_recentScroll;
        if (y + kRowH < kListTop || y > kListBottom) continue;
        const SystemEntry& se = m_systems[m_recent[i].sys];
        const Library::Game& g = se.games[m_recent[i].game];
        bool active = i == m_recentSel;
        FRect row{Theme::kMargin - 16.0f, y, w - 2.0f * (Theme::kMargin - 16.0f), kRowH};
        Theme::rowCard(r, row, active);
        float chipW = Theme::chipWidth(r, se.sys->shortName, 34.0f);
        Theme::chip(r, row.x + row.w - 20.0f - chipW, y + (kRowH - 34.0f) * 0.5f, se.sys->shortName,
                    Theme::alpha(Theme::badgeColor(se.sys->id), 200), {255, 255, 255, 255}, 34.0f);
        float tx = row.x + 24.0f, availW = row.w - 64.0f - chipW;
        Theme::rowTitle(r, g.title, tx, y + 12.0f, availW, Theme::Type::Body, active ? Theme::Text : Theme::TextDim, active);
        AppFont::draw(r, Theme::ellipsize(r, se.sys->name + (g.tags.empty() ? "" : "   " + g.tags), availW,
                                          Theme::Type::Caption, AppFont::Face::Body),
                      tx, y + 54.0f, Theme::Type::Caption, Theme::Muted);
    }
    endListClip();
    drawHeader("Consoles", "Recently played", m_recentSel + 1, (int)m_recent.size());
    Theme::footerHints(r, w, "A Play   X Search   B Back", "");
}

// Repaints the background behind the header band, then the header itself, so
// list rows scrolled up under it are covered.
void Menu::drawHeader(const std::string& caption, const std::string& title, int index, int total) {
    SDL_Renderer* r = m_renderer;
    const int w = AppConfig::kLogicalWidth;
    beginListClip(0.0f, kListTop - 4.0f);
    Theme::backgroundAnimated(r, w, AppConfig::kLogicalHeight, m_clock);
    endListClip();
    Theme::header(r, w, caption, title);
    if (total > 0) Theme::counter(r, w, index, total);
}

void Menu::renderToast() {
    if (m_toast.empty()) return;
    SDL_Renderer* r = m_renderer;
    const int w = AppConfig::kLogicalWidth;
    const float h = 76.0f, y = Theme::kFooterTop - h - 18.0f;
    FRect box{Theme::kMargin - 16.0f, y, w - 2.0f * (Theme::kMargin - 16.0f), h};
    Gfx::softRect(r, box, 22.0f, 18.0f, {0, 0, 0, 180}, false);
    Gfx::panel(r, box, 22.0f, {36, 42, 66, 250}, {24, 28, 46, 250}, Theme::alpha(Theme::accent(), 200), 2.0f);
    std::string text = Theme::ellipsize(r, m_toast, box.w - 48.0f, Theme::Type::Small, AppFont::Face::Body);
    AppFont::drawCentered(r, text, w * 0.5f, y + (h - Theme::Type::Small) * 0.5f - 4.0f, Theme::Type::Small, Theme::Text);
}

// The big letter shown while flipping through the alphabet.
void Menu::renderJump() {
    if (!m_jumpChar || m_jumpTime > 0.8f) return;
    SDL_Renderer* r = m_renderer;
    const float cx = AppConfig::kLogicalWidth * 0.5f, cy = 640.0f, s = 200.0f;
    float fade = m_jumpTime < 0.55f ? 1.0f : 1.0f - (m_jumpTime - 0.55f) / 0.25f;
    FRect box{cx - s * 0.5f, cy - s * 0.5f, s, s};
    Gfx::softRect(r, box, 40.0f, 30.0f, {0, 0, 0, (Uint8)(160 * fade)}, false);
    Gfx::panel(r, box, 40.0f, {30, 40, 66, (Uint8)(240 * fade)}, {20, 26, 46, (Uint8)(240 * fade)},
               Theme::alpha(Theme::accent(), (int)(220 * fade)), 3.0f);
    AppFont::drawCentered(r, std::string(1, m_jumpChar), cx, cy - 72.0f, 140.0f, Theme::alpha(Theme::Text, (int)(255 * fade)),
                          AppFont::Face::Display);
}

void Menu::render(float dt) {
    m_clock += dt;
    Theme::clock() = m_clock;
    m_jumpTime += dt;
    if (!m_toast.empty()) {
        m_toastTime += dt;
        if (m_toastTime > 6.0f) m_toast.clear();
    }
    if (m_view == View::Games) m_gameScroll = scrollFor(m_gameScroll, m_gameSel, (int)m_systems[sysIndex()].games.size(), dt);
    else if (m_view == View::Systems) m_sysScroll = scrollFor(m_sysScroll, m_sysRow, (int)m_systems.size() + kFixedRows, dt);
    else if (m_view == View::Recent) m_recentScroll = scrollFor(m_recentScroll, m_recentSel, (int)m_recent.size(), dt);
    else m_hitScroll = scrollFor(m_hitScroll, m_hitSel, (int)m_hits.size(), dt, kHitsTop, kListBottom, kHitH + kHitGap);

    SDL_SetRenderTarget(m_renderer, m_canvas);
    SDL_RenderSetScale(m_renderer, m_canvasScale, m_canvasScale);
    Theme::backgroundAnimated(m_renderer, AppConfig::kLogicalWidth, AppConfig::kLogicalHeight, m_clock);
    if (m_view == View::Games) renderGames();
    else if (m_view == View::Search) renderSearch();
    else if (m_view == View::Recent) renderRecent();
    else renderSystems();
    renderJump();
    renderToast();
    AppFont::drawRight(m_renderer, "v" APP_VERSION, AppConfig::kLogicalWidth - Theme::kMargin, 18.0f, 16.0f, Theme::Faint);
    SDL_RenderSetScale(m_renderer, 1.0f, 1.0f);
    SDL_SetRenderTarget(m_renderer, nullptr);
}

void Menu::present() {
    int ow = AppConfig::kFramebufferWidth, oh = AppConfig::kFramebufferHeight;
    SDL_GetRendererOutputSize(m_renderer, &ow, &oh);
    float scale = std::min(ow / (float)AppConfig::kLogicalHeight, oh / (float)AppConfig::kLogicalWidth);
    int cw = (int)std::lround(m_canvasW * scale / m_canvasScale), ch = (int)std::lround(m_canvasH * scale / m_canvasScale);
    SDL_Rect dst{ow / 2 - cw / 2, oh / 2 - ch / 2, cw, ch};
    SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
    SDL_RenderClear(m_renderer);
    SDL_RenderCopyEx(m_renderer, m_canvas, nullptr, &dst, AppConfig::kFirmwareRotationDegrees, nullptr, SDL_FLIP_NONE);
    SDL_RenderPresent(m_renderer);
}

int Menu::run() {
    Library::ensureFolders(m_appDir);
    m_settings.load(m_appDir);
    scan();
    // Return to where the player left off.
    for (int i = 0; i < (int)m_systems.size(); ++i)
        if (m_systems[i].sys->id == m_startSys) {
            m_sysRow = i + kFixedRows;
            if (!m_systems[i].games.empty()) {
                m_view = View::Games;
                m_gameSel = std::max(0, std::min(m_startIndex, (int)m_systems[i].games.size() - 1));
            }
        }
    if (!m_startSys.empty() || !m_toast.empty())
        log("menu resumes at %s #%d, message '%s'", m_startSys.c_str(), m_startIndex, m_toast.c_str());
    if (!initVideo()) return 1;
    if (m_startSys == "@recent") {
        loadRecent();
        m_sysRow = 1;
        if (!m_recent.empty()) {
            m_view = View::Recent;
            m_recentSel = std::max(0, std::min(m_startIndex, (int)m_recent.size() - 1));
            m_recentScroll = scrollFor(0.0f, m_recentSel, (int)m_recent.size(), 0.0f);
        }
    }
    m_sysScroll = scrollFor(0.0f, m_sysRow, (int)m_systems.size() + kFixedRows, 0.0f);
    m_gameScroll = m_view == View::Games ? scrollFor(0.0f, m_gameSel, (int)m_systems[sysIndex()].games.size(), 0.0f) : 0.0f;

    bool running = true;
    Uint32 last = SDL_GetTicks();
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;
            AtGames::ControlEvent ce = m_controls.event(ev, true, false);
            if (ce != AtGames::ControlEvent::None) handle(ce, running);
        }
        Uint32 now = SDL_GetTicks();
        float dt = std::min((now - last) / 1000.0f, 0.033f);
        last = now;
        render(dt);
        present();
        updatePanels(dt);
    }
    log("menu exit");
    shutdown();
    return 0;
}

} // namespace

int runMenu(const std::string& appDir, const std::string& sys, int index, const std::string& message) {
    Library::openLog(appDir, "menu");
    Menu menu(appDir, sys, index, message);
    return menu.run();
}
