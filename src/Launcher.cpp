#include "Launcher.h"

#include "AppConfig.h"
#include "AppFont.h"
#include "ArtDownload.h"
#include "Transfer.h"
#include "Trackball.h"
#include "Update.h"
#include "Arcade.h"
#include "DisplayProfile.h"
#include "GamePanels.h"
#include "Gfx.h"
#include "Library.h"
#include "Theme.h"
#include "Version.h"
#include "controls/Controls.h"

#include <SDL.h>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
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
    std::vector<int> shown;  // the games list as browsed: indexes into games (an arcade genre, or all)
    std::string filter;      // "" all, "\x01vertical", or a genre
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
    enum class View { Systems, Games, Search, Recent, Settings, Controls, Genres, Transfer, Bios };  // Recent also shows Favorites

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

    // Arcade genres: picking Arcade first lists All games, Vertical games and
    // each genre (MAME's catver.ini), and the games list shows that one.
    struct GenreRow { std::string key, label; int count; };
    std::vector<GenreRow> genreRows(int sys) const;
    bool hasGenres(int sys) const { return m_systems[sys].sys->id == "arcade"; }
    void applyFilter(int sys, const std::string& key);
    int m_genreSel = 0;
    float m_genreScroll = 0.0f;
    void renderGenres();
    int shownGame() const { return m_systems[sysIndex()].shown[m_gameSel]; }  // index into games
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
    int m_sysRow = 0;   // 0 Search, 1 Recently played, 2 Favorites, 3 Settings, 4.. = m_systems[m_rowSys[row - 4]]
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

    // The consoles listed, as m_systems indexes: all of them, or only those with
    // games when Settings > Hide consoles with no games is on.
    std::vector<int> m_rowSys;
    void rebuildRows();
    int sysIndex() const {
        int r = m_sysRow - kFixedRows;
        return r >= 0 && r < (int)m_rowSys.size() ? m_rowSys[r] : 0;
    }
    int rowOf(int sys) const {  // the list row showing m_systems[sys], or -1
        for (int k = 0; k < (int)m_rowSys.size(); ++k)
            if (m_rowSys[k] == sys) return k + kFixedRows;
        return -1;
    }
    static constexpr int kFixedRows = 4;  // Search, Recently played, Favorites, Settings
    int settingsRow() const { return 3; }
    int systemRows() const { return (int)m_rowSys.size() + kFixedRows; }

    // Favorites, as (system id, ROM file).
    std::set<std::pair<std::string, std::string>> m_favs;
    bool m_listIsFav = false;  // the Recent view is showing Favorites
    bool isFav(int sys, int game) const {
        return m_favs.count({m_systems[sys].sys->id, m_systems[sys].games[game].file}) > 0;
    }
    void toggleFav(int sys, int game);
    void openList(bool favorites);
    void cycleGameScreen(int sys, int game, int dir);
    ScreenId gameScreen(int sys, int game) const;

    // Settings screen.
    struct SettingDef { std::string label, key; std::vector<std::string> values, labels; };
    static const std::vector<SettingDef>& settingDefs();
    int m_setSel = 0;
    void renderSettings();
    void changeSetting(int dir);

    // Held directions repeat: polled each frame from every controller, the
    // left stick and the keyboard, so a hold works however it is reported.
    enum Dir { DirUp, DirDown, DirLeft, DirRight, DirCount };
    bool m_dirTap[DirCount] = {};    // a press event seen this frame
    bool m_dirHeld[DirCount] = {};
    bool m_repeating = false;        // the current event is a held repeat
    Uint32 m_dirSince[DirCount] = {}, m_dirNext[DirCount] = {};
    void pollDirections(bool& running);
    void moveSystem(int dir);

    // "Exit Retro Launcher?" on B at the consoles list.
    // A yes/no question over the menu: exit, remove a game, empty the trash.
    enum class Confirm { None, Exit, Remove, EmptyTrash, StopArt, Update } m_confirm = Confirm::None;
    int m_confirmSel = 0;  // 0 Cancel, 1 the action
    std::string m_confirmQ, m_confirmOk;
    void ask(Confirm what, const std::string& question, const std::string& ok) {
        m_confirm = what; m_confirmQ = question; m_confirmOk = ok; m_confirmSel = 0;
    }
    int m_removeSys = 0, m_removeGame = 0;
    void removeGame(int sys, int game);
    unsigned long long m_trashBytes = 0;  // measured when Settings opens and after changes

    // Settings > Download artwork (ArtDownload.h), after the setting rows and before Empty trash.
    ArtDownload m_art;
    std::string m_artLast;  // the last run's result, shown on the row
    int artRow() const { return (int)settingDefs().size(); }
    int transferRow() const { return (int)settingDefs().size() + 1; }
    int biosRow() const { return (int)settingDefs().size() + 2; }
    int updateRow() const { return (int)settingDefs().size() + 3; }
    int trashRow() const { return (int)settingDefs().size() + 4; }

    // The Arcade Control Panel's trackball (Trackball.h) scrolls the lists:
    // rolling it moves the selection a row per step, stopping at the ends.
    Trackball m_trackball;
    float m_trackballAcc = 0.0f;
    void trackballScroll(bool& running);

    // Settings > Updates (Update.h); checked by itself once a day at startup.
    Updater m_update;
    bool m_updateAnnounced = false;
    std::string updateValue() const;

    // Settings > BIOS check: what each system with games can use, and where it goes.
    struct BiosRow {
        std::string title, line, why, status;
        enum Kind { Ok, Missing, Optional, Check } kind = Ok;
    };
    std::vector<BiosRow> m_biosRows;
    int m_biosSel = 0;
    float m_biosScroll = 0.0f;
    int m_biosRequiredMissing = 0;
    void openBios();
    void renderBios();
    float m_setScroll = 0.0f;

    // Settings > Network transfer (Transfer.h): its own screen while the server runs.
    TransferServer m_transfer;
    View m_transferReturn = View::Settings;
    void openTransfer();
    void closeTransfer();
    void renderTransfer();
    void startArtDownload();

    // Home on a game: options popup.
    bool m_popup = false;
    int m_popupSys = 0, m_popupGame = 0, m_popupSel = 0;
    bool highlightedGame(int& sys, int& game) const;
    void handlePopup(AtGames::ControlEvent ev, bool& running);
    void renderPopup();

    // Console photos for the system rows, cut out on a worker thread (decoding
    // twelve full-size photos would hold up the first frame by seconds).
    struct ConsoleIcon { std::vector<uint8_t> px; int w = 0, h = 0; bool ok = false; SDL_Texture* tex = nullptr; };
    std::vector<ConsoleIcon> m_icons;
    std::thread m_iconThread;
    std::atomic<int> m_iconsDone{0};
    std::atomic<bool> m_iconStop{false};
    std::atomic<bool> m_iconsWarm{false};  // the worker has finished everything
    void startConsoleIcons();
    void drawRowIcon(int row, const FRect& slot, bool dim);
    enum { PopPlay, PopFav, PopScreen, PopCore, PopCvStart, PopControls, PopSearch, PopRemove };
    // ColecoVision on the firmware core: the keypad key tapped for you at the
    // "select game" screen (Player's auto-start), per game. "1" unless set.
    std::string cvStartKey(int sys, int game) const {
        return "cvstart." + m_systems[sys].sys->id + "/" + m_systems[sys].games[game].file;
    }
    void cycleCvStart(int sys, int game, int dir);

    // Controls screen (Home > Controls on a game): the button layout for this
    // game or its whole system, a preset, and press-to-assign per button.
    View m_ctlReturn = View::Systems;
    int m_ctlSys = 0, m_ctlGame = 0, m_ctlSel = 0;
    bool m_ctlGameScope = true, m_ctlCapture = false;
    float m_ctlCaptureTime = 0.0f, m_ctlScroll = 0.0f;
    void openControls(int sys, int game);
    std::string ctlFile() const { return m_ctlGameScope ? m_systems[m_ctlSys].games[m_ctlGame].file : ""; }
    Library::ButtonMap ctlMap() const;
    void ctlStore(const Library::ButtonMap& m);
    // Button names: ColecoVision's depend on which core will run it.
    std::string targetsId(const std::string& sys) const {
        return sys == "colecovision" && !Library::hasColecoBios(m_appDir) ? "colecovision:libcv" : sys;
    }
    int ctlRows() const { return 2 + (int)Library::buttonTargets(targetsId(m_systems[m_ctlSys].sys->id)).size() + 1; }
    void handleControls(AtGames::ControlEvent ev);
    void renderControls();
    std::vector<int> popupItems() const;  // PopCore only for arcade games
    // Arcade: the core a game plays with (its override, else the detected one),
    // and Auto -> each emulator that can run it, in the Home popup.
    std::string gameCore(int sys, int game) const;
    std::string artFor(int sys, const Library::Game& g) const;  // cover; arcade also by title
    std::string logoFor(int sys, const Library::Game& g) const;  // logo for the DMD, the same way
    void cycleGameCore(int sys, int game, int dir);
};

// ----------------------------------------------------------------- setup

// What the cabinet is, for planning GPU cores (Flycast, PPSSPP): the chip, its
// cores and memory, the firmware's SDL and the GL ES version behind its renderer.
static void logHardware(SDL_Renderer* renderer) {
    std::string compat;
    if (FILE* f = std::fopen("/proc/device-tree/compatible", "rb")) {
        char buf[256];
        size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        for (size_t i = 0; i < n; ++i) compat += buf[i] ? buf[i] : ' ';
    }
    int cpus = 0;
    std::map<std::string, int> parts;  // "CPU part" -> count (0xd0b A76, 0xd05 A55)
    if (FILE* f = std::fopen("/proc/cpuinfo", "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            if (std::strncmp(line, "processor", 9) == 0) ++cpus;
            if (std::strncmp(line, "CPU part", 8) == 0) {
                const char* v = std::strchr(line, ':');
                std::string part = v ? v + 2 : "?";
                while (!part.empty() && (part.back() == '\n' || part.back() == ' ')) part.pop_back();
                ++parts[part];
            }
        }
        std::fclose(f);
    }
    std::string partList;
    for (const auto& kv : parts) partList += (partList.empty() ? "" : ", ") + std::to_string(kv.second) + "x " + kv.first;
    long memMb = 0;
    if (FILE* f = std::fopen("/proc/meminfo", "r")) {
        long kb = 0;
        if (std::fscanf(f, "MemTotal: %ld kB", &kb) == 1) memMb = kb / 1024;
        std::fclose(f);
    }
    SDL_version v;
    SDL_GetVersion(&v);
    SDL_RendererInfo info{};
    SDL_GetRendererInfo(renderer, &info);
    log("hw: %s| %d cpus (%s), %ld MB, SDL %d.%d.%d, renderer %s", compat.c_str(), cpus, partList.c_str(), memMb,
        v.major, v.minor, v.patch, info.name ? info.name : "?");
    typedef const unsigned char* (*GetString)(unsigned);
    GetString glGetString = (GetString)SDL_GL_GetProcAddress("glGetString");
    if (glGetString && SDL_GL_GetCurrentContext()) {
        const unsigned char* ver = glGetString(0x1F02);   // GL_VERSION
        const unsigned char* rend = glGetString(0x1F01);  // GL_RENDERER
        const unsigned char* sl = glGetString(0x8B8C);    // GL_SHADING_LANGUAGE_VERSION
        log("hw: GL %s | %s | GLSL %s", ver ? (const char*)ver : "?", rend ? (const char*)rend : "?",
            sl ? (const char*)sl : "?");
    } else {
        log("hw: no GL context behind the renderer");
    }
}

bool Menu::initVideo() {
    m_topo = DisplayProfile::detect();
    log("model '%s' %s: main %u, backglass %u, dmd %u", m_topo.model.c_str(), m_topo.known ? "known" : "unknown",
        m_topo.main.connectorId, m_topo.backglass.connectorId, m_topo.dmd.connectorId);
    // The menu lives on the playfield. Set (or clear) both variables explicitly:
    // the environment carries over from the game process.
    ::unsetenv("SDL2_DISPLAY_PLANE_TYPE");
    ::unsetenv("ForceConnectID");
    DisplayProfile::prepareSdlMain(m_topo);

    // Never let an SDL assertion exit the app (its default handler does).
    SDL_SetAssertionHandler([](const SDL_AssertData* d, void*) -> SDL_AssertState {
        if (d && d->trigger_count == 0) log("SDL assertion '%s' at %s:%d - ignored", d->condition, d->filename, d->linenum);
        return SDL_ASSERTION_IGNORE;
    }, nullptr);
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
    Library::logInputDevices();
    logHardware(m_renderer);
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

std::string Menu::updateValue() const {
    switch (m_update.state()) {
    case Updater::State::Checking: return "Checking...";
    case Updater::State::UpToDate: return "Up to date (v" APP_VERSION ")";
    case Updater::State::NoRelease: return "No release yet";
    case Updater::State::Available: return "v" + m_update.latest() + " available";
    case Updater::State::Downloading: return "Downloading " + std::to_string(m_update.progress()) + "%";
    case Updater::State::Installing: return "Installing...";
    case Updater::State::Done: return "Restarting...";
    case Updater::State::Failed: return m_update.message();
    default: return "v" APP_VERSION " - check";
    }
}

void Menu::shutdown() {
    m_update.stop();
    m_transfer.stop();
    m_art.stop();  // a game is starting (or the menu is closing): the rest waits for next time
    if (m_iconThread.joinable()) {
        m_iconStop = true;
        // Finished: join. Mid-photo: let it go; exec is next and ends it.
        if (m_iconsDone.load() >= (int)m_icons.size() && m_iconsWarm) m_iconThread.join();
        else m_iconThread.detach();
    }
    for (ConsoleIcon& ic : m_icons)
        if (ic.tex) { SDL_DestroyTexture(ic.tex); ic.tex = nullptr; }
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
        SystemEntry e{&s, Library::scanGames(m_appDir, s), {}, ""};
        int renamed = 0;
        for (Library::Game& g : e.games) {
            if (g.arcade) continue;
            std::string nice = niceTitle(m_appDir, s.id, g.file);
            if (!nice.empty()) { g.title = nice; ++renamed; }
        }
        if (renamed)
            std::sort(e.games.begin(), e.games.end(), [](const Library::Game& a, const Library::Game& b) {
                std::string x = a.title, y = b.title;
                for (char& c : x) c = (char)std::tolower((unsigned char)c);
                for (char& c : y) c = (char)std::tolower((unsigned char)c);
                return x < y;
            });
        for (int i = 0; i < (int)e.games.size(); ++i) e.shown.push_back(i);
        if (Library::isArcadeSystem(s.id)) {  // hide clones / broken sets if Settings says so
            m_systems.push_back(std::move(e));
            applyFilter((int)m_systems.size() - 1, "");
            const SystemEntry& added = m_systems.back();
            log("%s: %zu game(s), %zu shown", s.id.c_str(), added.games.size(), added.shown.size());
            continue;
        }
        log("%s: %zu game(s)%s", s.id.c_str(), e.games.size(),
            renamed ? (", " + std::to_string(renamed) + " named from their covers").c_str() : "");
        m_systems.push_back(std::move(e));
    }
}

// ----------------------------------------------------------------- input

void Menu::trackballScroll(bool& running) {
    if (m_trackball.fd < 0) return;
    m_trackball.frame(true);
    // Not while Controls waits for a button to assign, or a roll would be taken for one.
    if (m_ctlCapture && m_view == View::Controls) { m_trackballAcc = 0.0f; return; }
    const std::string speed = m_settings.value("trackball.speed", "normal");
    const float perRow = speed == "slow" ? 48.0f : speed == "fast" ? 12.0f : 24.0f;  // trackball counts per row
    m_trackballAcc += m_trackball.dy;
    int steps = (int)(m_trackballAcc / perRow);
    m_trackballAcc -= steps * perRow;
    steps = std::max(-12, std::min(12, steps));  // a hard spin moves at most 12 rows a frame
    if (!steps) return;
    m_repeating = true;  // like a held direction: stop at the ends instead of wrapping
    for (int i = 0; i < std::abs(steps); ++i)
        handle(steps > 0 ? AtGames::ControlEvent::Down : AtGames::ControlEvent::Up, running);
    m_repeating = false;
}

void Menu::move(int delta) {
    // A single press wraps around; held repeats and flipper jumps stop at the ends.
    bool wrap = (delta == 1 || delta == -1) && !m_repeating;
    auto step = [delta, wrap](int cur, int n) {
        if (n <= 0) return 0;
        if (wrap) return (cur + delta + n) % n;
        return std::max(0, std::min(n - 1, cur + delta));
    };
    if (m_view == View::Systems) { m_sysRow = step(m_sysRow, systemRows()); return; }
    if (m_view == View::Settings) { m_setSel = step(m_setSel, trashRow() + 1); return; }  // + Download artwork, Empty trash
    if (m_view == View::Recent) { m_recentSel = step(m_recentSel, (int)m_recent.size()); return; }
    if (m_view == View::Games) m_gameSel = step(m_gameSel, (int)m_systems[sysIndex()].shown.size());
    if (m_view == View::Genres) m_genreSel = step(m_genreSel, (int)genreRows(sysIndex()).size());
}

// Flippers in a game list: jump to the start of the previous / next letter.
void Menu::jumpLetter(int dir) {
    const SystemEntry& se = m_systems[sysIndex()];
    struct Shown {
        const SystemEntry& e;
        const Library::Game& operator[](int i) const { return e.games[e.shown[i]]; }
    } games{se};
    int n = (int)se.shown.size();
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
    const ScreenId screen = gameScreen(sys, game);
    // Draw a "Starting" frame so the press registers before the screen goes dark.
    m_toast = "Starting " + g.title + "...";
    m_toastTime = 0.0f;
    render(0.0f);
    present();
    shutdown();
    Library::execPlay(m_appDir, e.sys->id, g.path, screen, returnIndex >= 0 ? returnIndex : game, returnTo,
                      g.arcade ? gameCore(sys, game) : "");
    // Only reached if exec failed: come back up.
    initVideo();
    m_toast = "Could not start the game (see data/launcher.log).";
}

// ------------------------------------------------- favorites / screens

void Menu::toggleFav(int sys, int game) {
    auto key = std::make_pair(m_systems[sys].sys->id, m_systems[sys].games[game].file);
    bool added = !m_favs.count(key);
    if (added) m_favs.insert(key);
    else m_favs.erase(key);
    Library::saveFavorites(m_appDir, {m_favs.begin(), m_favs.end()});
    m_toast = (added ? "Added to Favorites: " : "Removed from Favorites: ") + m_systems[sys].games[game].title;
    m_toastTime = 0.0f;
}

// Recently played (newest first) or Favorites (A-Z), in the same list view.
void Menu::openList(bool favorites) {
    m_listIsFav = favorites;
    if (favorites) {
        m_recent.clear();
        for (int s = 0; s < (int)m_systems.size(); ++s)
            for (int g = 0; g < (int)m_systems[s].games.size(); ++g)
                if (isFav(s, g)) m_recent.push_back({s, g});
        std::sort(m_recent.begin(), m_recent.end(), [&](const Hit& a, const Hit& b) {
            return lower(m_systems[a.sys].games[a.game].title) < lower(m_systems[b.sys].games[b.game].title);
        });
    } else {
        loadRecent();
    }
    m_recentSel = 0;
    m_recentScroll = 0.0f;
}

// Games play on the backglass or the playfield (never the DMD). Unless set per
// game, vertical arcade games take the playfield, which is portrait.
ScreenId Menu::gameScreen(int sys, int game) const {
    const Library::Game& g = m_systems[sys].games[game];
    ScreenId s;
    if (!m_settings.gameScreen(m_systems[sys].sys->id, g.file, s))
        s = g.vertical ? ScreenId::Playfield : m_settings.screenFor(m_systems[sys].sys->id, g.file);
    return s == ScreenId::Playfield ? ScreenId::Playfield : ScreenId::Backglass;
}

std::string Menu::artFor(int sys, const Library::Game& g) const {
    // A game in its own folder ("Dolphin Blue/disc.gdi") goes by the folder's name.
    size_t slash = g.file.find('/');
    std::string file = slash == std::string::npos ? g.file : g.file.substr(0, slash) + g.file.substr(g.file.find_last_of('.'));
    std::string p = findArt(m_appDir, m_systems[sys].sys->id, file);
    if (p.empty() && g.arcade) p = findArt(m_appDir, m_systems[sys].sys->id, g.title + ".zip");
    if (p.empty() && !g.altTitle.empty()) p = findArt(m_appDir, m_systems[sys].sys->id, g.altTitle + ".zip");
    return p;
}

// Every game's names as artFor tries them, for the downloader to match.
void Menu::startArtDownload() {
    std::vector<ArtDownload::System> work;
    for (const SystemEntry& e : m_systems) {
        if (e.games.empty()) continue;
        ArtDownload::System s;
        s.id = e.sys->id;
        for (const Library::Game& g : e.games) {
            size_t slash = g.file.find('/');
            ArtDownload::Game a;
            a.names.push_back(slash == std::string::npos ? g.file
                                                         : g.file.substr(0, slash) + g.file.substr(g.file.find_last_of('.')));
            if (g.arcade && !g.title.empty()) a.names.push_back(g.title + ".zip");
            if (!g.altTitle.empty()) a.names.push_back(g.altTitle + ".zip");
            s.games.push_back(std::move(a));
        }
        work.push_back(std::move(s));
    }
    if (work.empty()) { m_toast = "Add some games first"; m_toastTime = 0.0f; return; }
    m_art.start(m_appDir, std::move(work));
    m_toast = "Downloading artwork - keep browsing; starting a game stops it";
    m_toastTime = 0.0f;
}

std::string Menu::logoFor(int sys, const Library::Game& g) const {
    const std::string& id = m_systems[sys].sys->id;
    size_t slash = g.file.find('/');
    std::string file = slash == std::string::npos ? g.file : g.file.substr(0, slash) + g.file.substr(g.file.find_last_of('.'));
    std::string p = findLogo(m_appDir, id, file);
    if (p.empty() && (g.arcade || slash != std::string::npos || !g.title.empty())) p = findLogo(m_appDir, id, g.title + ".zip");
    if (p.empty() && !g.altTitle.empty()) p = findLogo(m_appDir, id, g.altTitle + ".zip");
    return p;
}

std::string Menu::gameCore(int sys, int game) const {
    const Library::Game& g = m_systems[sys].games[game];
    std::string over = m_settings.value("core." + m_systems[sys].sys->id + "/" + g.file, "");
    return !over.empty() ? over : g.core;
}

void Menu::cycleGameCore(int sys, int game, int dir) {
    const Library::Game& g = m_systems[sys].games[game];
    const std::string key = "core." + m_systems[sys].sys->id + "/" + g.file;
    std::vector<std::string> choices{""};
    choices.insert(choices.end(), g.cores.begin(), g.cores.end());
    std::string cur = m_settings.value(key, "");
    int i = 0;
    for (int k = 0; k < (int)choices.size(); ++k)
        if (choices[k] == cur) i = k;
    i = (i + dir + (int)choices.size()) % (int)choices.size();
    m_settings.set(key, choices[i]);
    m_settings.save();
}

void Menu::cycleCvStart(int sys, int game, int dir) {
    static const std::vector<std::string> choices{"off", "1", "2", "3", "4", "5", "6", "7", "8"};
    std::string cur = m_settings.value(cvStartKey(sys, game), "1");
    int i = 1;
    for (int k = 0; k < (int)choices.size(); ++k)
        if (choices[k] == cur) i = k;
    i = (i + dir + (int)choices.size()) % (int)choices.size();
    m_settings.set(cvStartKey(sys, game), choices[i]);
    m_settings.save();
}

std::vector<int> Menu::popupItems() const {
    std::vector<int> items{PopPlay, PopFav, PopScreen};
    if (m_systems[m_popupSys].games[m_popupGame].arcade) items.push_back(PopCore);
    if (targetsId(m_systems[m_popupSys].sys->id) == "colecovision:libcv") items.push_back(PopCvStart);
    items.push_back(PopControls);
    items.push_back(PopSearch);
    items.push_back(PopRemove);
    return items;
}

// Screen option in the Home popup: Default -> Backglass -> Playfield -> Default.
void Menu::cycleGameScreen(int sys, int game, int dir) {
    const std::string& id = m_systems[sys].sys->id;
    const std::string& file = m_systems[sys].games[game].file;
    ScreenId cur;
    int i = 0;  // 0 default, 1 backglass, 2 playfield
    if (m_settings.gameScreen(id, file, cur)) i = cur == ScreenId::Playfield ? 2 : 1;
    i = (i + dir + 3) % 3;
    if (i == 0) m_settings.clearGameScreen(id, file);
    else m_settings.setGameScreen(id, file, i == 2 ? ScreenId::Playfield : ScreenId::Backglass);
    m_settings.save();
}

// ----------------------------------------------------------------- settings

const std::vector<Menu::SettingDef>& Menu::settingDefs() {
    static const std::vector<SettingDef> defs = {
        {"Picture sides", "sides", {"bezel", "glow", "black"}, {"Bezel", "Glow", "Black"}},
        {"Scaling", "scaling", {"smooth", "sharp", "integer"}, {"Smooth", "Sharp", "Pixel-perfect"}},
        {"CRT scanlines", "scanlines", {"off", "light", "strong"}, {"Off", "Light", "Strong"}},
        {"Screen artwork", "panels", {"on", "off"}, {"On", "Off"}},
        {"Default game screen", "screen.default", {"backglass", "playfield"}, {"Backglass", "Playfield"}},
        {"Playfield game rotation", "rotate.playfield", {"90", "270"}, {"90 degrees", "270 degrees"}},
        {"Arcade: hide clones & broken sets", "arcade.hide", {"off", "on"}, {"Off", "On"}},
        {"Hide consoles with no games", "systems.hideEmpty", {"off", "on"}, {"Off", "On"}},
        {"Trackball speed", "trackball.speed", {"normal", "slow", "fast"}, {"Normal", "Slow", "Fast"}},
    };
    return defs;
}

void Menu::changeSetting(int dir) {
    const SettingDef& d = settingDefs()[m_setSel];
    std::string cur = m_settings.value(d.key, d.values[0]);
    // Older files: picture sides came from bezels=/bars=.
    if (d.key == "sides" && m_settings.value("sides", "").empty())
        cur = m_settings.value("bezels", "on") == "off" ? (m_settings.value("bars", "ambient") == "black" ? "black" : "glow")
                                                        : "bezel";
    auto it = std::find(d.values.begin(), d.values.end(), cur);
    int i = it == d.values.end() ? 0 : (int)(it - d.values.begin());
    int n = (int)d.values.size();
    m_settings.set(d.key, d.values[(i + dir + n) % n]);
    m_settings.save();
    if (d.key == "panels") {
        m_toast = "Screen artwork changes on the next start";
        m_toastTime = 0.0f;
    }
    if (d.key == "systems.hideEmpty") rebuildRows();
    if (d.key == "arcade.hide") {  // re-filter the arcade lists now
        int hidden = 0;
        for (int i = 0; i < (int)m_systems.size(); ++i)
            if (Library::isArcadeSystem(m_systems[i].sys->id)) {
                applyFilter(i, m_systems[i].filter);
                for (const Library::Game& g : m_systems[i].games) hidden += g.clone || g.broken;
            }
        if (m_settings.value("arcade.hide", "off") == "on") {
            m_toast = std::to_string(hidden) + " arcade sets hidden (clones and broken sets)";
            m_toastTime = 0.0f;
        }
    }
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
    if (ev == CE::B || ev == CE::Back) { back(); return; }
    if (ev == CE::LeftShoulder) { if (!m_query.empty()) { m_query.pop_back(); runQuery(); } return; }
    if (ev == CE::RightShoulder) { if (!m_hits.empty()) m_inHits = true; return; }
    if (m_inHits) {
        if (m_hits.empty()) { m_inHits = false; return; }
        int n = (int)m_hits.size();
        const Hit& h = m_hits[m_hitSel];
        if (ev == CE::Up) { if (m_hitSel > 0) --m_hitSel; else if (!m_repeating) m_inHits = false; }
        else if (ev == CE::Down) m_hitSel = std::min(n - 1, m_hitSel + 1);
        else if (ev == CE::Rewind || ev == CE::Rewind2 || ev == CE::Y) toggleFav(h.sys, h.game);
        else if (ev == CE::Guide) { m_popup = true; m_popupSys = h.sys; m_popupGame = h.game; m_popupSel = 0; }
        else if (ev == CE::A || ev == CE::Start) launch(h.sys, h.game);
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

// The game under the cursor in a games list, Recent / Favorites or search results.
bool Menu::highlightedGame(int& sys, int& game) const {
    if (m_view == View::Games && !m_systems[sysIndex()].shown.empty()) { sys = sysIndex(); game = shownGame(); return true; }
    if (m_view == View::Recent && !m_recent.empty()) { sys = m_recent[m_recentSel].sys; game = m_recent[m_recentSel].game; return true; }
    if (m_view == View::Search && m_inHits && !m_hits.empty()) { sys = m_hits[m_hitSel].sys; game = m_hits[m_hitSel].game; return true; }
    return false;
}

// Second flippers in a games list: previous / next system that has games.
void Menu::rebuildRows() {
    const int cur = m_sysRow >= kFixedRows ? sysIndex() : -1;
    const bool hide = m_settings.value("systems.hideEmpty", "off") == "on";
    m_rowSys.clear();
    for (int i = 0; i < (int)m_systems.size(); ++i)
        if (!hide || !m_systems[i].games.empty()) m_rowSys.push_back(i);
    if (cur >= 0) {
        int r = rowOf(cur);
        m_sysRow = r >= 0 ? r : std::max(0, std::min(m_sysRow, systemRows() - 1));
    }
}

void Menu::moveSystem(int dir) {
    int n = (int)m_rowSys.size(), cur = m_sysRow - kFixedRows;
    for (int k = 1; k < n; ++k) {
        int r = ((cur + dir * k) % n + n) % n, i = m_rowSys[r];
        if (m_systems[i].games.empty()) continue;
        m_sysRow = r + kFixedRows;
        applyFilter(i, hasGenres(i) ? m_settings.value("genre." + m_systems[i].sys->id, "") : "");
        m_gameSel = 0;
        m_gameScroll = 0.0f;
        return;
    }
}

void Menu::handlePopup(AtGames::ControlEvent ev, bool& running) {
    using CE = AtGames::ControlEvent;
    const std::vector<int> items = popupItems();
    const int count = (int)items.size();
    m_popupSel = std::min(m_popupSel, count - 1);
    const int item = items[m_popupSel];
    switch (ev) {
    case CE::Up: if (m_popupSel > 0) --m_popupSel; else if (!m_repeating) m_popupSel = count - 1; break;
    case CE::Down: if (m_popupSel < count - 1) ++m_popupSel; else if (!m_repeating) m_popupSel = 0; break;
    case CE::Left:
    case CE::Right:
        if (item == PopScreen) cycleGameScreen(m_popupSys, m_popupGame, ev == CE::Left ? -1 : 1);
        if (item == PopCore) cycleGameCore(m_popupSys, m_popupGame, ev == CE::Left ? -1 : 1);
        if (item == PopCvStart) cycleCvStart(m_popupSys, m_popupGame, ev == CE::Left ? -1 : 1);
        break;
    case CE::B: case CE::Back: case CE::Guide: m_popup = false; break;
    case CE::Rewind: case CE::Rewind2: case CE::Y:
        m_popup = false;
        handle(ev, running);
        break;
    case CE::A:
    case CE::Start:
        switch (item) {
        case PopPlay: m_popup = false; handle(CE::A, running); break;
        case PopFav: m_popup = false; handle(CE::Rewind, running); break;
        case PopScreen: cycleGameScreen(m_popupSys, m_popupGame, 1); break;
        case PopCore: cycleGameCore(m_popupSys, m_popupGame, 1); break;
        case PopCvStart: cycleCvStart(m_popupSys, m_popupGame, 1); break;
        case PopControls: m_popup = false; openControls(m_popupSys, m_popupGame); break;
        case PopRemove:
            m_popup = false;
            m_removeSys = m_popupSys;
            m_removeGame = m_popupGame;
            ask(Confirm::Remove, "Remove this game?", "Move to trash");
            break;
        case PopSearch: m_popup = false; openSearch(); break;
        }
        break;
    default: break;
    }
}

void Menu::handle(AtGames::ControlEvent ev, bool& running) {
    using CE = AtGames::ControlEvent;
    if (m_confirm != Confirm::None) {
        if (ev == CE::Up || ev == CE::Down) m_confirmSel ^= 1;
        else if (ev == CE::B || ev == CE::Back) m_confirm = Confirm::None;
        else if (ev == CE::A || ev == CE::Start) {
            Confirm what = m_confirm;
            m_confirm = Confirm::None;
            if (m_confirmSel == 1) {
                if (what == Confirm::Exit) running = false;
                else if (what == Confirm::Remove) removeGame(m_removeSys, m_removeGame);
                else if (what == Confirm::StopArt) m_art.cancel();
                else if (what == Confirm::Update) m_update.install();
                else if (what == Confirm::EmptyTrash) {
                    m_toast = Library::emptyTrash(m_appDir) ? "Trash emptied" : "Some files could not be removed";
                    m_toastTime = 0.0f;
                    m_trashBytes = Library::trashSize(m_appDir);
                }
            }
        }
        return;
    }
    if (m_popup) { handlePopup(ev, running); return; }
    if (m_view == View::Search) { handleSearch(ev); return; }
    if (m_view == View::Controls) { handleControls(ev); return; }
    if (m_view == View::Transfer) {
        if (ev == CE::B || ev == CE::Back || ev == CE::Guide || ev == CE::A || ev == CE::Start) closeTransfer();
        return;
    }
    if (m_view == View::Bios) {
        const int n = (int)m_biosRows.size();
        if (ev == CE::B || ev == CE::Back || ev == CE::Guide) { m_view = View::Settings; m_panelKey.clear(); }
        else if (n && ev == CE::Up) m_biosSel = m_biosSel > 0 ? m_biosSel - 1 : (m_repeating ? 0 : n - 1);
        else if (n && ev == CE::Down) m_biosSel = m_biosSel < n - 1 ? m_biosSel + 1 : (m_repeating ? n - 1 : 0);
        else if (n && (ev == CE::LeftShoulder || ev == CE::RightShoulder))
            m_biosSel = std::max(0, std::min(n - 1, m_biosSel + (ev == CE::LeftShoulder ? -4 : 4)));
        return;
    }
    int hs = 0, hg = 0;
    bool onGame = highlightedGame(hs, hg);
    switch (ev) {
    case CE::Up: move(-1); break;
    case CE::Down: move(1); break;
    case CE::LeftShoulder:
        if (m_view == View::Games) jumpLetter(-1); else move(-4);
        break;
    case CE::RightShoulder:
        if (m_view == View::Games) jumpLetter(1); else move(4);
        break;
    case CE::LeftTrigger:
    case CE::RightTrigger:
        if (m_view == View::Games) moveSystem(ev == CE::LeftTrigger ? -1 : 1);
        break;
    case CE::Left:
    case CE::Right:
        // Only where there is something to the side: a setting's value.
        if (m_view == View::Settings && m_setSel < (int)settingDefs().size()) changeSetting(ev == CE::Left ? -1 : 1);
        break;
    case CE::Guide:
        if (onGame) { m_popup = true; m_popupSys = hs; m_popupGame = hg; m_popupSel = 0; }
        else if (m_view == View::Systems) openSearch();
        break;
    case CE::X:
        if (m_view != View::Settings) openSearch();
        break;
    case CE::Y:
    case CE::Rewind:
    case CE::Rewind2:
        // Favorite the highlighted game. Nothing anywhere else.
        if (m_view == View::Games && onGame) { toggleFav(hs, hg); break; }
        if (m_view == View::Recent && onGame) {
            toggleFav(hs, hg);
            if (m_listIsFav) {  // it left the list
                int keep = m_recentSel;
                openList(true);
                m_recentSel = std::min(keep, std::max(0, (int)m_recent.size() - 1));
                if (m_recent.empty()) m_view = View::Systems;
            }
        }
        break;
    case CE::A:
    case CE::Start:
        if (m_view == View::Settings) {
            if (m_setSel < (int)settingDefs().size()) changeSetting(1);
            else if (m_setSel == transferRow()) openTransfer();
            else if (m_setSel == biosRow()) openBios();
            else if (m_setSel == updateRow()) {
                const Updater::State us = m_update.state();
                if (us == Updater::State::Available)
                    ask(Confirm::Update, "Install v" + m_update.latest() + "?", "Install and restart");
                else if (us != Updater::State::Checking && us != Updater::State::Downloading &&
                         us != Updater::State::Installing && us != Updater::State::Done)
                    m_update.check(m_appDir, false);
            }
            else if (m_setSel == artRow()) {
                if (m_art.running()) ask(Confirm::StopArt, "Stop downloading artwork?", "Stop");
                else startArtDownload();
            }
            else if (m_trashBytes == 0) { m_toast = "The trash is empty"; m_toastTime = 0.0f; }
            else ask(Confirm::EmptyTrash, "Empty the trash?", "Delete for good");
            break;
        }
        if (m_view == View::Systems) {
            if (m_sysRow == 0) { openSearch(); break; }
            if (m_sysRow == 1 || m_sysRow == 2) {
                openList(m_sysRow == 2);
                if (m_recent.empty()) {
                    m_toast = m_sysRow == 2 ? "No favorites yet - press REWIND on a game to add it"
                                            : "Nothing played yet - games you start will show up here";
                    m_toastTime = 0.0f;
                } else {
                    m_view = View::Recent;
                }
                break;
            }
            if (m_sysRow == settingsRow()) {
                m_view = View::Settings;
                m_setSel = 0;
                m_setScroll = 0.0f;
                m_trashBytes = Library::trashSize(m_appDir);
                break;
            }
            SystemEntry& e = m_systems[sysIndex()];
            if (e.games.empty()) {
                m_toast = "Add " + e.sys->name + " ROMs to roms/" + e.sys->id + "/";
                m_toastTime = 0.0f;
            } else if (hasGenres(sysIndex())) {
                // Arcade: pick a genre first, starting on the one used last.
                m_view = View::Genres;
                auto rows = genreRows(sysIndex());
                m_genreSel = 0;
                for (int k = 0; k < (int)rows.size(); ++k)
                    if (rows[k].key == e.filter) m_genreSel = k;
                m_genreScroll = 0.0f;
            } else {
                m_view = View::Games;
                m_gameSel = 0;
                m_gameScroll = 0.0f;
            }
        } else if (m_view == View::Genres) {
            auto rows = genreRows(sysIndex());
            if (m_genreSel < (int)rows.size()) {
                applyFilter(sysIndex(), rows[m_genreSel].key);
                m_settings.set("genre." + m_systems[sysIndex()].sys->id, rows[m_genreSel].key);
                m_settings.save();
                m_view = View::Games;
                m_gameSel = 0;
                m_gameScroll = 0.0f;
            }
        } else if (m_view == View::Recent) {
            // The game moves to the top of the list, so come back to row 0.
            if (!m_recent.empty()) {
                if (m_listIsFav) launch(m_recent[m_recentSel].sys, m_recent[m_recentSel].game, "@favorites", m_recentSel);
                else launch(m_recent[m_recentSel].sys, m_recent[m_recentSel].game, "@recent", 0);
            }
        } else {
            launch(sysIndex(), shownGame());
        }
        break;
    case CE::B:
    case CE::Back:
        if (m_view == View::Systems) ask(Confirm::Exit, "Exit Retro Launcher?", "Exit");
        else if (m_view == View::Games && hasGenres(sysIndex())) m_view = View::Genres;  // back to the genre list
        else m_view = View::Systems;
        break;
    default: break;
    }
}

void Menu::updatePanels(float dt) {
    if (!m_panels || !m_panels->active()) return;
    std::string key;
    if (m_view == View::Bios) {  // the highlighted file and why it matters
        key = "bios:" + std::to_string(m_biosSel);
        if (key != m_panelKey) {
            if (m_biosRows.empty()) m_panelItem = {key, "BIOS check", "", "Nothing needed", ""};
            else {
                const BiosRow& b = m_biosRows[std::min(m_biosSel, (int)m_biosRows.size() - 1)];
                m_panelItem = {key, "BIOS check", "", b.status + " - " + b.why, ""};
            }
        }
        m_panelKey = key;
        m_panels->show(m_panelItem, dt);
        return;
    }
    if (m_view == View::Transfer) {  // the address and PIN, big, on the backglass
        TransferServer::Status st = m_transfer.status();
        key = "transfer:" + std::to_string(st.received) + (st.listening ? "" : ":off");
        if (key != m_panelKey) {
            std::string where = st.listening && !st.addresses.empty()
                                    ? st.addresses[0] + ":" + std::to_string(st.port) + "   PIN " + st.pin
                                    : "Not available";
            m_panelItem = {key, "Network transfer", "", where, ""};
        }
        m_panelKey = key;
        m_panels->show(m_panelItem, dt);
        return;
    }
    int sys = -1, game = -1;
    if (m_view == View::Games && !m_systems[sysIndex()].shown.empty()) { sys = sysIndex(); game = shownGame(); }
    else if (m_view == View::Search && m_inHits && !m_hits.empty()) { sys = m_hits[m_hitSel].sys; game = m_hits[m_hitSel].game; }
    else if (m_view == View::Recent && !m_recent.empty()) { sys = m_recent[m_recentSel].sys; game = m_recent[m_recentSel].game; }

    if (sys >= 0 && game >= 0 && game < (int)m_systems[sys].games.size()) {
        const Library::Game& g = m_systems[sys].games[game];
        key = "game:" + m_systems[sys].sys->id + "/" + g.file;
        if (key != m_panelKey) {
            m_panelItem = {key, m_systems[sys].sys->name, g.title, g.tags,
                           artFor(sys, g)};
            m_panelItem.logoPath = logoFor(sys, g);
        }
    } else if (m_view == View::Search) {
        key = "search:" + std::to_string(m_hits.size());
        if (key != m_panelKey) {
            m_panelItem = {key, "Search", "", m_hits.empty() ? "Type to find a game" :
                           std::to_string(m_hits.size()) + (m_hits.size() == 1 ? " match" : " matches"), ""};
            m_panelItem.icon = "search";
        }
    } else if (m_sysRow == 0) {
        key = "row:search";
        if (key != m_panelKey) { m_panelItem = {key, "Search", "", "Find any game", ""}; m_panelItem.icon = "search"; }
    } else if (m_view == View::Settings || (m_view == View::Systems && m_sysRow == settingsRow())) {
        key = "row:settings";
        if (key != m_panelKey) { m_panelItem = {key, "Settings", "", "Display and screens", ""}; m_panelItem.icon = "settings"; }
    } else if (m_sysRow == 1 || m_sysRow == 2) {
        key = m_sysRow == 1 ? "row:recent" : "row:favorites";
        if (key != m_panelKey) {
            m_panelItem = {key, m_sysRow == 1 ? "Recently played" : "Favorites", "",
                           m_sysRow == 1 ? "Your last games" : std::to_string(m_favs.size()) + " games", ""};
            m_panelItem.icon = m_sysRow == 1 ? "recent" : "favorites";
        }
    } else {
        const SystemEntry& e = m_systems[sysIndex()];
        key = "sys:" + e.sys->id;
        if (key != m_panelKey)
            m_panelItem = {key, e.sys->name, "",
                           e.games.empty() ? "No games yet" : std::to_string(e.games.size()) + " games", ""};
        if (key != m_panelKey) m_panelItem.consolePath = findConsoleArt(m_appDir, e.sys->id);  // for the DMD
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

// Right side of a game row: a heart for favorites and a screen chip. The chip
// shows on the highlighted row, and on any row whose screen was chosen for it
// (accent color). Returns the width it took.
float drawGameExtras(SDL_Renderer* r, const FRect& row, bool active, bool fav, bool overridden, ScreenId screen) {
    float right = row.x + row.w - 20.0f, used = 20.0f;
    if (active || overridden) {
        std::string label = Library::screenLabel(screen);
        float cw = Theme::chipWidth(r, label, 32.0f);
        SDL_Color fill = overridden ? Theme::accent() : SDL_Color{255, 255, 255, 34};
        SDL_Color ink = overridden ? Theme::onAccent(Theme::accent()) : Theme::TextDim;
        Theme::chip(r, right - cw, row.y + (row.h - 32.0f) * 0.5f, label, fill, ink, 32.0f);
        right -= cw + 12.0f;
        used += cw + 12.0f;
    }
    if (fav) {
        Gfx::heart(r, right - 14.0f, row.y + row.h * 0.5f, 26.0f, Theme::Rose);
        used += 36.0f;
    }
    return used;
}

// Photo slot on a system row, in canvas pixels (3x the 104x72 logical slot).
constexpr float kIconSlotW = 104.0f, kIconSlotH = 72.0f;

// Console cut-outs are cached in data/icons/<system>.rgba (keyed by the photo's
// size and date and the slot size), so only the first start pays for decoding
// full-size photos and removing their backgrounds.
namespace {
struct IconCacheHeader { char magic[4]; uint32_t w, h, slotW, slotH; int64_t srcSize, srcTime; };

bool loadIconCache(const std::string& path, const IconCacheHeader& want, std::vector<uint8_t>& px, int& w, int& h) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    IconCacheHeader got{};
    bool ok = std::fread(&got, sizeof(got), 1, f) == 1 && std::memcmp(got.magic, "RLI1", 4) == 0 &&
              got.slotW == want.slotW && got.slotH == want.slotH && got.srcSize == want.srcSize &&
              got.srcTime == want.srcTime && got.w > 0 && got.h > 0 && got.w <= 4096 && got.h <= 4096;
    if (ok) {
        px.resize((size_t)got.w * got.h * 4);
        ok = std::fread(px.data(), 1, px.size(), f) == px.size();
        w = (int)got.w; h = (int)got.h;
    }
    std::fclose(f);
    return ok;
}

void saveIconCache(const std::string& path, IconCacheHeader head, const std::vector<uint8_t>& px, int w, int h) {
    std::memcpy(head.magic, "RLI1", 4);
    head.w = (uint32_t)w; head.h = (uint32_t)h;
    std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    bool ok = std::fwrite(&head, sizeof(head), 1, f) == 1 && std::fwrite(px.data(), 1, px.size(), f) == px.size();
    std::fclose(f);
    if (ok) std::rename(tmp.c_str(), path.c_str());
    else std::remove(tmp.c_str());
}
} // namespace

void Menu::startConsoleIcons() {
    m_icons.assign(m_systems.size(), ConsoleIcon());
    m_iconsDone = 0;
    m_iconStop = false;
    int maxW = (int)(kIconSlotW * m_canvasScale), maxH = (int)(kIconSlotH * m_canvasScale);
    std::vector<std::string> ids;
    for (const SystemEntry& e : m_systems) ids.push_back(e.sys->id);
    m_iconThread = std::thread([this, ids, maxW, maxH]() {
        Uint32 t0 = SDL_GetTicks();
        int found = 0, cached = 0;
        const std::string cacheDir = m_appDir + "/data/icons";
        ::mkdir(cacheDir.c_str(), 0755);
        for (size_t i = 0; i < ids.size() && !m_iconStop; ++i) {
            ConsoleIcon& ic = m_icons[i];
            std::string photo = findConsoleArt(m_appDir, ids[i]);
            struct stat st;
            if (!photo.empty() && ::stat(photo.c_str(), &st) == 0) {
                IconCacheHeader head{};
                head.slotW = (uint32_t)maxW; head.slotH = (uint32_t)maxH;
                head.srcSize = (int64_t)st.st_size; head.srcTime = (int64_t)st.st_mtime;
                const std::string cache = cacheDir + "/" + ids[i] + ".rgba";
                if (loadIconCache(cache, head, ic.px, ic.w, ic.h)) {
                    ic.ok = true;
                    ++cached;
                } else {
                    ic.ok = consoleCutout(m_appDir, ids[i], maxW, maxH, ic.px, ic.w, ic.h);
                    if (ic.ok) saveIconCache(cache, head, ic.px, ic.w, ic.h);
                }
            }
            found += ic.ok;
            m_iconsDone.store((int)i + 1, std::memory_order_release);
        }
        log("console icons: %d of %zu (%d from cache) in %u ms", found, ids.size(), cached, SDL_GetTicks() - t0);
        t0 = SDL_GetTicks();
        for (size_t i = 0; i < ids.size() && !m_iconStop; ++i) warmArtIndex(m_appDir, ids[i]);
        if (!m_iconStop) log("cover index ready in %u ms", SDL_GetTicks() - t0);
        m_iconsWarm = true;
    });
}

// The picture at the left of a consoles-list row: an icon for the fixed rows,
// the console's photo for a system (a gamepad until it is ready, or without one).
void Menu::drawRowIcon(int row, const FRect& slot, bool dim) {
    SDL_Renderer* r = m_renderer;
    const float is = 56.0f, ix = slot.x + (slot.w - is) * 0.5f, iy = slot.y + (slot.h - is) * 0.5f;
    if (row == 0) { Theme::icon(r, Theme::Icon::Search, ix, iy, is, Theme::Accent); return; }
    if (row == 1) { Theme::icon(r, Theme::Icon::Clock, ix, iy, is, Theme::Accent2); return; }
    if (row == 2) { Theme::icon(r, Theme::Icon::Heart, ix, iy, is, Theme::Rose); return; }
    if (row == settingsRow()) { Theme::icon(r, Theme::Icon::Gear, ix, iy, is, {150, 160, 185, 255}); return; }
    int i = m_rowSys[row - kFixedRows];
    if (i < m_iconsDone.load(std::memory_order_acquire)) {
        ConsoleIcon& ic = m_icons[i];
        if (ic.ok && !ic.tex) {
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
            ic.tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, ic.w, ic.h);
            if (ic.tex) {
                SDL_UpdateTexture(ic.tex, nullptr, ic.px.data(), ic.w * 4);
                SDL_SetTextureBlendMode(ic.tex, SDL_BLENDMODE_BLEND);
            } else {
                ic.ok = false;
            }
            std::vector<uint8_t>().swap(ic.px);
        }
        if (ic.tex) {
            float w = ic.w / m_canvasScale, h = ic.h / m_canvasScale;
            // Integer SDL_RenderCopy: the cabinet's SDL has no SDL_RenderCopyF (2.0.10+),
            // though the SDK headers declare it; it fails at the first call.
            SDL_Rect dst{(int)std::lround(slot.x + (slot.w - w) * 0.5f), (int)std::lround(slot.y + (slot.h - h) * 0.5f),
                         (int)std::lround(w), (int)std::lround(h)};
            SDL_SetTextureAlphaMod(ic.tex, dim ? 90 : 255);
            SDL_RenderCopy(r, ic.tex, nullptr, &dst);
            return;
        }
    }
    SDL_Color c = dim ? SDL_Color{90, 98, 120, 255} : Theme::badgeColor(m_systems[i].sys->id);
    Theme::icon(r, Library::isArcadeSystem(m_systems[i].sys->id) ? Theme::Icon::Joystick : Theme::Icon::Gamepad, ix, iy, is, c);
}

void Menu::renderSystems() {
    SDL_Renderer* r = m_renderer;
    const int w = AppConfig::kLogicalWidth;
    beginListClip(kListTop, kListBottom);
    for (int i = 0; i < systemRows(); ++i) {
        float y = kListTop + i * (kRowH + kRowGap) - m_sysScroll;
        if (y + kRowH < kListTop || y > kListBottom) continue;
        bool active = i == m_sysRow;
        FRect row{Theme::kMargin - 16.0f, y, w - 2.0f * (Theme::kMargin - 16.0f), kRowH};
        Theme::rowCard(r, row, active);
        float tx = row.x + 140.0f;
        FRect slot{row.x + 18.0f, y + (kRowH - kIconSlotH) * 0.5f, kIconSlotW, kIconSlotH};
        drawRowIcon(i, slot, i >= kFixedRows && i != settingsRow() && m_systems[m_rowSys[i - kFixedRows]].games.empty());
        if (i == 0) {
            AppFont::draw(r, "Search", tx, y + 14.0f, Theme::Type::Body, active ? Theme::Text : Theme::TextDim);
            AppFont::draw(r, "Find any game on any system", tx, y + 54.0f, Theme::Type::Caption, Theme::Muted);
            continue;
        }
        if (i == 2) {
            AppFont::draw(r, "Favorites", tx, y + 14.0f, Theme::Type::Body, active ? Theme::Text : Theme::TextDim);
            AppFont::draw(r, m_favs.empty() ? "Press REWIND on a game to add it" : std::to_string(m_favs.size()) + " games",
                          tx, y + 54.0f, Theme::Type::Caption, Theme::Muted);
            continue;
        }
        if (i == settingsRow()) {
            AppFont::draw(r, "Settings", tx, y + 14.0f, Theme::Type::Body, active ? Theme::Text : Theme::TextDim);
            AppFont::draw(r, "Picture, scaling, scanlines, screens", tx, y + 54.0f, Theme::Type::Caption, Theme::Muted);
            continue;
        }
        if (i == 1) {
            AppFont::draw(r, "Recently played", tx, y + 14.0f, Theme::Type::Body, active ? Theme::Text : Theme::TextDim);
            AppFont::draw(r, "Your last 20 games", tx, y + 54.0f, Theme::Type::Caption, Theme::Muted);
            continue;
        }
        const SystemEntry& e = m_systems[m_rowSys[i - kFixedRows]];
        bool empty = e.games.empty();
        SDL_Color tc = empty ? Theme::Faint : (active ? Theme::Text : Theme::TextDim);
        AppFont::draw(r, e.sys->name, tx, y + 14.0f, Theme::Type::Body, tc);
        std::string sub = empty ? "No games - add ROMs to roms/" + e.sys->id + "/"
                                : std::to_string(e.games.size()) + (e.games.size() == 1 ? " game" : " games");
        AppFont::draw(r, sub, tx, y + 54.0f, Theme::Type::Caption, empty ? Theme::Faint : Theme::Muted);
    }
    endListClip();
    drawHeader("Retro Launcher", "Consoles", m_sysRow + 1, systemRows());
    Theme::footerHints(r, w, "A Open   HOME Search   B Exit", "");
}

// Home > Remove game (confirmed): its files go to trash/<system>/, the lists
// forget it. An arcade set other sets need (a parent) stays.
void Menu::removeGame(int sys, int game) {
    SystemEntry& e = m_systems[sys];
    if (game < 0 || game >= (int)e.games.size()) return;
    const Library::Game g = e.games[game];
    if (g.arcade) {
        std::string set = g.file.substr(0, g.file.size() - 4);
        for (char& c : set) c = (char)std::tolower((unsigned char)c);
        int needed = 0;
        std::string example;
        for (const Library::Game& o : e.games)
            if (o.parent == set) { if (!needed) example = o.title; ++needed; }
        if (needed) {
            m_toast = "Kept: " + std::to_string(needed) + " other game" + (needed == 1 ? "" : "s") + " need it (" + example + ")";
            m_toastTime = 0.0f;
            return;
        }
    }
    std::string error;
    if (!Library::moveToTrash(m_appDir, e.sys->id, Library::gameParts(m_appDir, e.sys->id, g), error)) {
        m_toast = "Could not remove it: " + error;
        m_toastTime = 0.0f;
        return;
    }
    if (m_favs.erase({e.sys->id, g.file})) Library::saveFavorites(m_appDir, {m_favs.begin(), m_favs.end()});
    e.games.erase(e.games.begin() + game);
    if (Library::isArcadeSystem(e.sys->id)) applyFilter(sys, e.filter);
    else {
        e.shown.clear();
        for (int i = 0; i < (int)e.games.size(); ++i) e.shown.push_back(i);
    }
    if (m_view == View::Games) {
        m_gameSel = std::min(m_gameSel, std::max(0, (int)e.shown.size() - 1));
        if (e.shown.empty()) m_view = hasGenres(sys) && !e.games.empty() ? View::Genres : View::Systems;
        if (e.games.empty()) rebuildRows();  // its last game: hidden now if empty consoles are
    } else if (m_view == View::Recent) {
        openList(m_listIsFav);
        m_recentSel = std::min(m_recentSel, std::max(0, (int)m_recent.size() - 1));
        if (m_recent.empty()) m_view = View::Systems;
    } else if (m_view == View::Search) {
        runQuery();
        m_hitSel = std::min(m_hitSel, std::max(0, (int)m_hits.size() - 1));
        if (m_hits.empty()) m_inHits = false;
    }
    m_trashBytes = Library::trashSize(m_appDir);
    m_toast = "Moved to the trash: " + g.title;
    m_toastTime = 0.0f;
}

// Arcade genre rows: All, Vertical, then genres by size (small ones folded
// into Other). Counts respect the hide-clones-and-broken setting.
std::vector<Menu::GenreRow> Menu::genreRows(int sys) const {
    const SystemEntry& e = m_systems[sys];
    const bool hide = m_settings.value("arcade.hide", "off") == "on";
    std::map<std::string, int> counts;
    int all = 0, vertical = 0;
    for (const Library::Game& g : e.games) {
        if (hide && (g.clone || g.broken)) continue;
        ++all;
        if (g.vertical) ++vertical;
        ++counts[g.genre];
    }
    std::vector<GenreRow> genres;
    int other = 0;
    for (const auto& kv : counts) {
        if (kv.first.empty() || kv.second < 5) { other += kv.second; continue; }
        static const std::map<std::string, std::string> plural = {
            {"Shooter", "Shooters"}, {"Fighter", "Fighters"}, {"Platform", "Platformers"}, {"Maze", "Maze games"},
            {"Driving", "Driving"}, {"Sports", "Sports"}, {"Puzzle", "Puzzle"}, {"Ball & Paddle", "Ball & paddle"}};
        auto pl = plural.find(kv.first);
        genres.push_back({kv.first, pl != plural.end() ? pl->second : kv.first, kv.second});
    }
    std::sort(genres.begin(), genres.end(), [](const GenreRow& a, const GenreRow& b) { return a.count > b.count; });
    std::vector<GenreRow> rows{{"", "All games", all}};
    if (vertical) rows.push_back({"\x01vertical", "Vertical games", vertical});
    rows.insert(rows.end(), genres.begin(), genres.end());
    if (other) rows.push_back({"\x01other", "Other", other});
    return rows;
}

void Menu::applyFilter(int sys, const std::string& key) {
    SystemEntry& e = m_systems[sys];
    const bool hide = Library::isArcadeSystem(e.sys->id) && m_settings.value("arcade.hide", "off") == "on";
    // "Other" holds what genreRows folded away: no genre, or a genre under 5 games.
    std::map<std::string, int> counts;
    if (key == "\x01other")
        for (const Library::Game& g : e.games)
            if (!(hide && (g.clone || g.broken))) ++counts[g.genre];
    e.filter = key;
    e.shown.clear();
    for (int i = 0; i < (int)e.games.size(); ++i) {
        const Library::Game& g = e.games[i];
        if (hide && (g.clone || g.broken)) continue;
        bool in = key.empty() || (key == "\x01vertical" && g.vertical) ||
                  (key == "\x01other" && (g.genre.empty() || counts[g.genre] < 5)) || g.genre == key;
        if (in) e.shown.push_back(i);
    }
    if (e.shown.empty() && !key.empty()) applyFilter(sys, "");  // a genre that no longer has games
}

void Menu::renderGenres() {
    SDL_Renderer* r = m_renderer;
    const int w = AppConfig::kLogicalWidth;
    const auto rows = genreRows(sysIndex());
    beginListClip(kListTop, kListBottom);
    for (int i = 0; i < (int)rows.size(); ++i) {
        float y = kListTop + i * (kRowH + kRowGap) - m_genreScroll;
        if (y + kRowH < kListTop || y > kListBottom) continue;
        bool active = i == m_genreSel;
        FRect row{Theme::kMargin - 16.0f, y, w - 2.0f * (Theme::kMargin - 16.0f), kRowH};
        Theme::rowCard(r, row, active);
        AppFont::draw(r, rows[i].label, row.x + 24.0f, y + 14.0f, Theme::Type::Body, active ? Theme::Text : Theme::TextDim);
        AppFont::draw(r, std::to_string(rows[i].count) + (rows[i].count == 1 ? " game" : " games"), row.x + 24.0f,
                      y + 54.0f, Theme::Type::Caption, Theme::Muted);
    }
    endListClip();
    drawHeader("Genres", m_systems[sysIndex()].sys->name, m_genreSel + 1, (int)rows.size());
    Theme::footerHints(r, w, "A Open   B Back", "");
}

void Menu::renderGames() {
    SDL_Renderer* r = m_renderer;
    const int w = AppConfig::kLogicalWidth;
    const SystemEntry& e = m_systems[sysIndex()];
    beginListClip(kListTop, kListBottom);
    for (int i = 0; i < (int)e.shown.size(); ++i) {
        float y = kListTop + i * (kRowH + kRowGap) - m_gameScroll;
        if (y + kRowH < kListTop || y > kListBottom) continue;
        const int gi = e.shown[i];
        const Library::Game& g = e.games[gi];
        bool active = i == m_gameSel;
        FRect row{Theme::kMargin - 16.0f, y, w - 2.0f * (Theme::kMargin - 16.0f), kRowH};
        Theme::rowCard(r, row, active);
        ScreenId over;
        bool overridden = m_settings.gameScreen(e.sys->id, g.file, over);
        float extras = drawGameExtras(r, row, active, isFav(sysIndex(), gi), overridden, gameScreen(sysIndex(), gi));
        float tx = row.x + 24.0f, availW = row.w - 28.0f - extras;
        // Arcade sets that are incomplete stay listed, dimmer, with the reason.
        SDL_Color tc = active ? Theme::Text : g.problem.empty() ? Theme::TextDim : Theme::Faint;
        Theme::rowTitle(r, g.title, tx, y + 12.0f, availW, Theme::Type::Body, tc, active);
        if (!g.tags.empty())
            AppFont::draw(r, Theme::ellipsize(r, g.tags, availW, Theme::Type::Caption, AppFont::Face::Body), tx, y + 54.0f,
                          Theme::Type::Caption, Theme::Muted);
    }
    endListClip();
    std::string caption = "Games";
    if (hasGenres(sysIndex()))
        for (const GenreRow& gr : genreRows(sysIndex()))
            if (gr.key == e.filter) caption = gr.label;
    drawHeader(caption, e.sys->name, m_gameSel + 1, (int)e.shown.size());
    Theme::footerHints(r, w, "A Play   HOME Options   REWIND Favorite   B Back", "");
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
        if (isFav(m_hits[i].sys, m_hits[i].game))
            Gfx::heart(r, row.x + row.w - 36.0f - chipW, y + kHitH * 0.5f, 22.0f, Theme::Rose);
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
        // A key of n units spans the n-1 gaps inside it, so lay out per unit.
        float unitW = (fullW - kKeyGap * (total - 1.0f)) / total;
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
    Theme::trackedCentered(r, m_query.empty() ? "RESULTS" : (std::to_string(m_hits.size()) + (m_hits.size() >= 500 ? "+" : "") +
                                                             (m_hits.size() == 1 ? " MATCH" : " MATCHES")),
                           w * 0.5f, kHitsTop - 34.0f, Theme::Type::Caption, Theme::accent());
    if (m_hits.empty() && !m_query.empty())
        AppFont::drawCentered(r, "No games match", w * 0.5f, kHitsTop + 30.0f, Theme::Type::Small, Theme::Muted);

    drawHeader("Search", "All systems", 0, 0);
    Theme::footerHints(r, w, m_inHits ? "A Play   HOME Options   REWIND Favorite   B Keyboard" : "A Type   LB Delete   RB Results   B Back", "");
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
        ScreenId over;
        bool overridden = m_settings.gameScreen(se.sys->id, g.file, over);
        FRect er{row.x, row.y, row.w - chipW - 16.0f, row.h};
        float extras = drawGameExtras(r, er, active, isFav(m_recent[i].sys, m_recent[i].game), overridden,
                                      gameScreen(m_recent[i].sys, m_recent[i].game));
        float tx = row.x + 24.0f, availW = row.w - 44.0f - chipW - extras;
        Theme::rowTitle(r, g.title, tx, y + 12.0f, availW, Theme::Type::Body, active ? Theme::Text : Theme::TextDim, active);
        AppFont::draw(r, Theme::ellipsize(r, se.sys->name + (g.tags.empty() ? "" : "   " + g.tags), availW,
                                          Theme::Type::Caption, AppFont::Face::Body),
                      tx, y + 54.0f, Theme::Type::Caption, Theme::Muted);
    }
    endListClip();
    drawHeader("Consoles", m_listIsFav ? "Favorites" : "Recently played", m_recentSel + 1, (int)m_recent.size());
    Theme::footerHints(r, w, "A Play   HOME Options   REWIND Favorite   B Back", "");
}

void Menu::renderSettings() {
    SDL_Renderer* r = m_renderer;
    const int w = AppConfig::kLogicalWidth;
    const std::vector<SettingDef>& defs = settingDefs();
    const float step = kRowH + kRowGap;
    beginListClip(kListTop, kListBottom);
    auto rowAt = [&](int i, bool& active) {
        active = i == m_setSel;
        float y = kListTop + i * step - m_setScroll;
        FRect row{Theme::kMargin - 16.0f, y, w - 2.0f * (Theme::kMargin - 16.0f), kRowH};
        Theme::rowCard(r, row, active);
        return row;
    };
    // The actions after the settings: a label and a value on the right.
    auto actionRow = [&](int i, const std::string& label, std::string value, bool lit) {
        bool active;
        FRect row = rowAt(i, active);
        float ty = row.y + (kRowH - Theme::Type::Body) * 0.5f - 4.0f;
        AppFont::draw(r, label, row.x + 24.0f, ty, Theme::Type::Body, active ? Theme::Text : Theme::TextDim);
        float room = row.w - 72.0f - AppFont::measureWidth(r, label, Theme::Type::Body);
        value = Theme::ellipsize(r, value, room, Theme::Type::Body, AppFont::Face::Body);
        AppFont::drawRight(r, value, row.x + row.w - 24.0f, ty, Theme::Type::Body, lit || active ? Theme::accent() : Theme::Muted);
    };
    for (int i = 0; i < (int)defs.size(); ++i) {
        const SettingDef& d = defs[i];
        bool active;
        FRect row = rowAt(i, active);
        const float y = row.y;
        std::string cur = m_settings.value(d.key, d.values[0]);
        if (d.key == "sides" && m_settings.value("sides", "").empty())
            cur = m_settings.value("bezels", "on") == "off"
                      ? (m_settings.value("bars", "ambient") == "black" ? "black" : "glow") : "bezel";
        auto it = std::find(d.values.begin(), d.values.end(), cur);
        std::string label = d.labels[it == d.values.end() ? 0 : it - d.values.begin()];
        AppFont::draw(r, d.label, row.x + 24.0f, y + (kRowH - Theme::Type::Body) * 0.5f - 4.0f, Theme::Type::Body,
                      active ? Theme::Text : Theme::TextDim);
        // Value with arrows either side when highlighted.
        float vw = AppFont::measureWidth(r, label, Theme::Type::Body);
        float vx = row.x + row.w - 24.0f - vw - (active ? 30.0f : 0.0f);
        AppFont::draw(r, label, vx, y + (kRowH - Theme::Type::Body) * 0.5f - 4.0f, Theme::Type::Body,
                      active ? Theme::accent() : Theme::Muted);
        if (active) {
            Gfx::triangle(r, {vx - 30.0f, y + kRowH * 0.5f - 9.0f, 18.0f, 18.0f}, 180.0, Theme::accent());
            Gfx::triangle(r, {row.x + row.w - 42.0f, y + kRowH * 0.5f - 9.0f, 18.0f, 18.0f}, 0.0, Theme::accent());
        }
    }
    // Download artwork: covers and logos for games without them.
    actionRow(artRow(), "Download artwork", m_art.running() ? m_art.status() : "Missing only", m_art.running());
    // Network transfer: send games from a computer or phone (Transfer.h).
    actionRow(transferRow(), "Network transfer", "Open", false);
    // BIOS check: which BIOS files the systems with games can use, and where they go.
    actionRow(biosRow(), "BIOS check", "Open", false);
    // Updates: the latest GitHub release, installed in place.
    {
        const Updater::State us = m_update.state();
        actionRow(updateRow(), "Updates", updateValue(),
                  us == Updater::State::Available || us == Updater::State::Downloading || us == Updater::State::Installing);
    }
    // Empty trash: games removed with Home > Remove game wait in trash/ until then.
    char size[32];
    if (m_trashBytes == 0) std::snprintf(size, sizeof(size), "Empty");
    else if (m_trashBytes < 1024ull * 1024 * 1024) std::snprintf(size, sizeof(size), "%.0f MB", m_trashBytes / 1048576.0);
    else std::snprintf(size, sizeof(size), "%.1f GB", m_trashBytes / 1073741824.0);
    actionRow(trashRow(), "Empty trash", size, false);
    // Notes after the last row (scrolled into view with it).
    const float below = kListTop + (trashRow() + 1) * step - m_setScroll;
    AppFont::drawCentered(r, "Changes apply to the next game you start", w * 0.5f, below + 20.0f, Theme::Type::Caption,
                          Theme::Muted);
    AppFont::drawCentered(r, "Retro Launcher v" APP_VERSION, w * 0.5f, below + 56.0f, Theme::Type::Caption, Theme::Faint);
    endListClip();
    drawHeader("Retro Launcher", "Settings", m_setSel + 1, trashRow() + 1);
    Theme::footerHints(r, w, "LEFT/RIGHT Change   A Select   B Back", "");
}

// ----------------------------------------------------------------- BIOS check

void Menu::openBios() {
    std::vector<std::string> withGames;
    for (const SystemEntry& e : m_systems)
        if (!e.games.empty()) withGames.push_back(e.sys->id);
    m_biosRows.clear();
    m_biosRequiredMissing = 0;
    std::string report = "Retro Launcher BIOS check (Settings > BIOS check), for the systems with games.\n"
                         "Retro Launcher ships no BIOS files: use your own dumps, named as below.\n\n";
    for (const Library::BiosCheck& c : Library::checkBios(m_appDir, withGames)) {
        const Library::System* sys = Library::findSystem(c.system);
        BiosRow r;
        r.title = (sys ? sys->name : c.system) + ":  " + c.file;
        r.why = c.why;
        const char* need = c.required ? "Required" : "Good to have";
        if (c.state == Library::BiosCheck::Ok) {
            r.kind = BiosRow::Ok;
            r.status = "Found";
            r.line = std::string(need) + "  -  found in " + c.foundAt;
        } else if (c.state == Library::BiosCheck::Unrecognized) {
            r.kind = BiosRow::Check;
            r.status = "Unknown dump";
            r.line = std::string(need) + "  -  " + c.foundAt + " is not the known good dump";
        } else {
            r.kind = c.required ? BiosRow::Missing : BiosRow::Optional;
            r.status = c.required ? "Missing" : "Not added";
            r.line = std::string(need) + "  -  put it in " + c.where;
            if (c.required) ++m_biosRequiredMissing;
        }
        report += std::string(need) + ": " + (sys ? sys->name : c.system) + " - " + c.file + " - " + r.status +
                  (c.foundAt.empty() ? " - put it in " + c.where : " (" + c.foundAt + ")") + "\n    " + c.why + "\n";
        m_biosRows.push_back(r);
    }
    // Arcade sets that can't run for a missing BIOS or parent zip, from the scan.
    for (const SystemEntry& e : m_systems) {
        if (!Library::isArcadeSystem(e.sys->id)) continue;
        std::map<std::string, int> needs;
        for (const Library::Game& g : e.games)
            if (g.problem.compare(0, 6, "Needs ") == 0) ++needs[g.problem.substr(6)];
        for (const auto& kv : needs) {
            BiosRow r;
            r.title = e.sys->name + ":  " + kv.first;
            bool listed = false;  // already a row above (neogeo.zip, naomi.zip...)
            for (const BiosRow& o : m_biosRows) listed = listed || o.title == r.title;
            if (listed) continue;
            r.kind = BiosRow::Missing;
            r.status = "Missing";
            r.why = "a BIOS or parent set " + std::to_string(kv.second) + (kv.second == 1 ? " game needs" : " games need");
            r.line = "Required for " + std::to_string(kv.second) + (kv.second == 1 ? " game" : " games") + "  -  put it in roms/" +
                     e.sys->id + "/";
            ++m_biosRequiredMissing;
            report += "Required: " + e.sys->name + " - " + kv.first + " - Missing - put it in roms/" + e.sys->id + "/\n    " +
                      r.why + "\n";
            m_biosRows.push_back(r);
        }
    }
    if (m_biosRows.empty()) report += "Nothing: your systems need no BIOS files.\n";
    std::ofstream(m_appDir + "/data/bios-report.txt") << report;
    m_biosSel = 0;
    m_biosScroll = 0.0f;
    m_view = View::Bios;
    m_panelKey.clear();
}

void Menu::renderBios() {
    SDL_Renderer* r = m_renderer;
    const int w = AppConfig::kLogicalWidth;
    beginListClip(kListTop, kListBottom);
    if (m_biosRows.empty())
        AppFont::drawCentered(r, "Your systems need no BIOS files", w * 0.5f, kListTop + 40.0f, Theme::Type::Body, Theme::TextDim);
    for (int i = 0; i < (int)m_biosRows.size(); ++i) {
        const BiosRow& b = m_biosRows[i];
        const float y = kListTop + i * (kRowH + kRowGap) - m_biosScroll;
        if (y + kRowH < kListTop || y > kListBottom) continue;
        const bool active = i == m_biosSel;
        FRect row{Theme::kMargin - 16.0f, y, w - 2.0f * (Theme::kMargin - 16.0f), kRowH};
        Theme::rowCard(r, row, active);
        const SDL_Color fill = b.kind == BiosRow::Ok ? Theme::Accent : b.kind == BiosRow::Missing ? Theme::Rose
                             : b.kind == BiosRow::Check ? Theme::Gold : SDL_Color{80, 90, 112, 255};
        const float chipW = Theme::chipWidth(r, b.status, 34.0f);
        Theme::chip(r, row.x + row.w - 20.0f - chipW, y + 14.0f, b.status, fill, Theme::onAccent(fill), 34.0f);
        const float tx = row.x + 24.0f, avail = row.w - 60.0f - chipW;
        AppFont::draw(r, Theme::ellipsize(r, b.title, avail, Theme::Type::Body, AppFont::Face::Body), tx, y + 12.0f,
                      Theme::Type::Body, active ? Theme::Text : Theme::TextDim);
        AppFont::draw(r, Theme::ellipsize(r, b.line, row.w - 48.0f, Theme::Type::Caption, AppFont::Face::Body), tx, y + 56.0f,
                      Theme::Type::Caption, b.kind == BiosRow::Missing ? Theme::Rose : Theme::Muted);
    }
    endListClip();
    drawHeader("Settings", "BIOS check", m_biosRows.empty() ? 0 : m_biosSel + 1, (int)m_biosRows.size());
    Theme::footerHints(r, w, m_biosRequiredMissing ? std::to_string(m_biosRequiredMissing) + " required missing   B Back"
                                                   : "Nothing required is missing   B Back", "");
}

// ----------------------------------------------------------------- Network transfer

void Menu::openTransfer() {
    m_transferReturn = m_view;
    m_view = View::Transfer;
    m_transfer.start(m_appDir);
}

void Menu::closeTransfer() {
    const TransferServer::Status st = m_transfer.status();
    m_transfer.stop();
    m_view = m_transferReturn;
    if (st.received == 0) return;
    // New games: read the folders again (indexes into the lists may change).
    scan();
    rebuildRows();
    m_hits.clear();
    m_inHits = false;
    m_hitSel = 0;
    m_recent.clear();
    m_panelKey.clear();
    m_toast = "Added " + std::to_string(st.received) + (st.received == 1 ? " file" : " files") + " to your games";
    m_toastTime = 0.0f;
}

void Menu::renderTransfer() {
    SDL_Renderer* r = m_renderer;
    const int w = AppConfig::kLogicalWidth;
    const float cx = w * 0.5f, inner = w - 2.0f * Theme::kMargin;
    const TransferServer::Status st = m_transfer.status();
    float y = kListTop + 10.0f;
    if (!st.listening) {
        AppFont::drawCentered(r, "Couldn't start the transfer", cx, y, Theme::Type::Heading, Theme::Text, AppFont::Face::Display);
        AppFont::drawCentered(r, Theme::ellipsize(r, st.error, inner, Theme::Type::Small, AppFont::Face::Body), cx, y + 64.0f,
                              Theme::Type::Small, Theme::Muted);
    } else if (st.addresses.empty()) {
        AppFont::drawCentered(r, "No network connection", cx, y, Theme::Type::Heading, Theme::Text, AppFont::Face::Display);
        AppFont::drawCentered(r, "Connect a network cable or Wi-Fi, then try again.", cx, y + 64.0f,
                              Theme::Type::Small, Theme::Muted);
    } else {
        AppFont::drawCentered(r, "On a computer or phone on the same network, open", cx, y, Theme::Type::Small, Theme::Muted);
        y += 46.0f;
        for (size_t i = 0; i < st.addresses.size() && i < 2; ++i) {
            const std::string url = "http://" + st.addresses[i] + (st.port == 80 ? "" : ":" + std::to_string(st.port));
            AppFont::drawCentered(r, url, cx, y, Theme::Type::Heading, Theme::accent(), AppFont::Face::Display);
            y += 58.0f;
        }
        y += 20.0f;
        AppFont::drawCentered(r, "and enter the PIN", cx, y, Theme::Type::Small, Theme::Muted);
        y += 40.0f;
        AppFont::drawCentered(r, st.pin, cx, y, Theme::Type::Hero, Theme::Text, AppFont::Face::Display);
    }
    // What is happening.
    y = 640.0f;
    FRect card{Theme::kMargin - 16.0f, y, w - 2.0f * (Theme::kMargin - 16.0f), 470.0f};
    Theme::rowCard(r, card, false);
    float ty = y + 26.0f;
    const float tx = card.x + 28.0f, tw = card.w - 56.0f;
    if (!st.current.empty()) {
        AppFont::draw(r, "Receiving", tx, ty, Theme::Type::Caption, Theme::Muted);
        ty += 32.0f;
        AppFont::draw(r, Theme::ellipsize(r, st.current, tw, Theme::Type::Body, AppFont::Face::Body), tx, ty, Theme::Type::Body, Theme::Text);
        ty += 50.0f;
        const float f = st.currentTotal ? (float)((double)st.currentDone / (double)st.currentTotal) : 0.0f;
        Gfx::roundRect(r, {tx, ty, tw, 12.0f}, 6.0f, {255, 255, 255, 30});
        if (f > 0.0f) Gfx::roundRect(r, {tx, ty, std::max(12.0f, tw * f), 12.0f}, 6.0f, Theme::accent());
        char pct[48];
        std::snprintf(pct, sizeof(pct), "%d%%  of %.1f MB", (int)(f * 100.0f), st.currentTotal / 1048576.0);
        AppFont::draw(r, pct, tx, ty + 22.0f, Theme::Type::Caption, Theme::Muted);
        ty += 70.0f;
    } else {
        AppFont::draw(r, st.listening ? "Waiting for games" : "Not running", tx, ty, Theme::Type::Body, Theme::TextDim);
        ty += 60.0f;
    }
    if (st.received > 0) {
        char done[64];
        std::snprintf(done, sizeof(done), "Received %d %s (%.1f MB)", st.received, st.received == 1 ? "file" : "files",
                      st.receivedBytes / 1048576.0);
        AppFont::draw(r, done, tx, ty, Theme::Type::Small, Theme::accent());
        ty += 44.0f;
    }
    for (const std::string& line : st.recent) {
        if (ty > card.y + card.h - 40.0f) break;
        AppFont::draw(r, Theme::ellipsize(r, line, tw, Theme::Type::Small, AppFont::Face::Body), tx, ty, Theme::Type::Small,
                      line.find(" - failed") != std::string::npos ? Theme::Rose : Theme::TextDim);
        ty += 38.0f;
    }
    drawHeader("Settings", "Network transfer", 0, 0);
    Theme::footerHints(r, w, st.received ? "B Done - adds the new games" : "B Done", "");
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

// Home on a game: Play, Favorite, Screen, Search.
void Menu::renderPopup() {
    SDL_Renderer* r = m_renderer;
    const int w = AppConfig::kLogicalWidth, h = AppConfig::kLogicalHeight;
    const SystemEntry& se = m_systems[m_popupSys];
    const Library::Game& g = se.games[m_popupGame];
    Gfx::rect(r, {0.0f, 0.0f, (float)w, (float)h}, {4, 6, 12, 200});
    const float itemH = 96.0f, gap = 16.0f;
    const std::vector<int> items = popupItems();
    const int count = (int)items.size();
    const float pw = w - 2.0f * Theme::kMargin - 32.0f, ph = 190.0f + count * (itemH + gap);
    const float px = (w - pw) * 0.5f, py = (h - ph) * 0.5f - 40.0f;
    Gfx::softRect(r, {px, py, pw, ph}, 32.0f, 40.0f, {0, 0, 0, 200}, false);
    Gfx::panel(r, {px, py, pw, ph}, 32.0f, {30, 36, 58, 255}, {18, 22, 38, 255}, {255, 255, 255, 26}, 1.0f);
    AppFont::drawCentered(r, Theme::ellipsize(r, g.title, pw - 80.0f, Theme::Type::Heading, AppFont::Face::Display),
                          w * 0.5f, py + 40.0f, Theme::Type::Heading, Theme::Text, AppFont::Face::Display);
    AppFont::drawCentered(r, se.sys->name, w * 0.5f, py + 104.0f, Theme::Type::Caption, Theme::Muted);
    ScreenId over;
    bool overridden = m_settings.gameScreen(se.sys->id, g.file, over);
    const char* shown = gameScreen(m_popupSys, m_popupGame) == ScreenId::Playfield ? "Playfield" : "Backglass";
    std::string screen = !overridden ? std::string("Default (") + shown + ")" : shown;
    std::string coreOver = m_settings.value("core." + se.sys->id + "/" + g.file, "");
    std::string core = coreOver.empty() ? "Auto (" + Arcade::coreLabel(g.core) + ")" : Arcade::coreLabel(coreOver);
    for (int i = 0; i < count; ++i) {
        const int it = items[i];
        std::string label = it == PopPlay ? "Play" : it == PopFav ? (isFav(m_popupSys, m_popupGame) ? "Remove from Favorites" : "Add to Favorites")
                          : it == PopScreen ? "Screen" : it == PopCore ? "Emulator" : it == PopCvStart ? "Start with"
                          : it == PopControls ? "Controls" : it == PopRemove ? "Remove game" : "Search";
        FRect row{px + 40.0f, py + 160.0f + i * (itemH + gap), pw - 80.0f, itemH};
        bool active = i == m_popupSel;
        Theme::rowCard(r, row, active);
        float ty = row.y + (itemH - Theme::Type::Body) * 0.5f - 4.0f;
        AppFont::draw(r, label, row.x + 28.0f, ty, Theme::Type::Body, active ? Theme::Text : Theme::TextDim);
        if (it == PopScreen || it == PopCore || it == PopCvStart) {
            const std::string cv = m_settings.value(cvStartKey(m_popupSys, m_popupGame), "1");
            const std::string value = it == PopScreen ? screen : it == PopCore ? core
                                    : cv == "off" ? std::string("Off") : "Keypad " + cv;
            float vw = AppFont::measureWidth(r, value, Theme::Type::Body);
            float vx = row.x + row.w - 28.0f - vw - (active ? 30.0f : 0.0f);
            AppFont::draw(r, value, vx, ty, Theme::Type::Body, active ? Theme::accent() : Theme::Muted);
            if (active) {
                Gfx::triangle(r, {vx - 30.0f, row.y + itemH * 0.5f - 9.0f, 18.0f, 18.0f}, 180.0, Theme::accent());
                Gfx::triangle(r, {row.x + row.w - 46.0f, row.y + itemH * 0.5f - 9.0f, 18.0f, 18.0f}, 0.0, Theme::accent());
            }
        }
    }
}

// ----------------------------------------------------------------- controls

void Menu::openControls(int sys, int game) {
    m_ctlReturn = m_view;
    m_ctlSys = sys;
    m_ctlGame = game;
    int scope = 0;
    Library::buttonMapFor(m_settings, m_systems[sys].sys->id, m_systems[sys].games[game].file, &scope);
    m_ctlGameScope = scope == 2;  // start where the layout in force lives (a game's own, else the system's)
    m_ctlSel = 0;
    m_ctlCapture = false;
    m_ctlScroll = 0.0f;
    m_view = View::Controls;
}

Library::ButtonMap Menu::ctlMap() const {
    return Library::buttonMapFor(m_settings, m_systems[m_ctlSys].sys->id, ctlFile());
}

void Menu::ctlStore(const Library::ButtonMap& m) {
    m_settings.set(Library::buttonMapKey(m_systems[m_ctlSys].sys->id, ctlFile()), Library::buttonMapToString(m));
    m_settings.save();
}

void Menu::handleControls(AtGames::ControlEvent ev) {
    using CE = AtGames::ControlEvent;
    using Library::Cab;
    const std::string sysId = m_systems[m_ctlSys].sys->id;
    const auto targets = Library::buttonTargets(targetsId(sysId));
    const int rows = ctlRows(), resetRow = rows - 1;
    if (m_ctlCapture) {
        // The next cabinet button pressed goes to this emulated button. Home
        // cancels; directions are never remapped.
        Cab c = ev == CE::A ? Cab::A : ev == CE::B ? Cab::B : ev == CE::X ? Cab::X : ev == CE::Y ? Cab::Y
              : ev == CE::LeftShoulder ? Cab::LB : ev == CE::RightShoulder ? Cab::RB
              : ev == CE::LeftTrigger ? Cab::LB2 : ev == CE::RightTrigger ? Cab::RB2
              : ev == CE::Start ? Cab::Start : ev == CE::Rewind ? Cab::Rewind : ev == CE::Rewind2 ? Cab::Rewind2 : Cab::None;
        if (ev == CE::Guide) { m_ctlCapture = false; return; }
        if (c == Cab::None) return;
        Library::ButtonMap m = ctlMap();
        int id = targets[m_ctlSel - 2].first;
        Cab old = m.src[id];
        for (int i = 0; i < 16; ++i)  // the button's old job moves to whatever had this one (a swap)
            if (i != id && m.src[i] == c) m.src[i] = old;
        m.src[id] = c;
        ctlStore(m);
        m_ctlCapture = false;
        return;
    }
    auto presetStep = [&](int dir) {
        auto presets = Library::buttonPresets(sysId);
        Library::ButtonMap cur = ctlMap();
        int at = -1;
        for (int i = 0; i < (int)presets.size(); ++i)
            if (presets[i].map == cur) at = i;
        int next = at < 0 ? (dir > 0 ? 0 : (int)presets.size() - 1) : (at + dir + (int)presets.size()) % (int)presets.size();
        ctlStore(presets[next].map);
    };
    switch (ev) {
    case CE::Up: if (m_ctlSel > 0) --m_ctlSel; else if (!m_repeating) m_ctlSel = rows - 1; break;
    case CE::Down: if (m_ctlSel < rows - 1) ++m_ctlSel; else if (!m_repeating) m_ctlSel = 0; break;
    case CE::Left:
    case CE::Right:
        if (m_ctlSel == 0) m_ctlGameScope = !m_ctlGameScope;
        else if (m_ctlSel == 1) presetStep(ev == CE::Left ? -1 : 1);
        break;
    case CE::A:
    case CE::Start:
        if (m_ctlSel == 0) m_ctlGameScope = !m_ctlGameScope;
        else if (m_ctlSel == 1) presetStep(1);
        else if (m_ctlSel == resetRow) {
            m_settings.erase(Library::buttonMapKey(sysId, ctlFile()));
            m_settings.save();
            m_toast = m_ctlGameScope ? "This game now uses the " + m_systems[m_ctlSys].sys->name + " layout"
                                     : m_systems[m_ctlSys].sys->name + " back to the default layout";
            m_toastTime = 0.0f;
        } else {
            m_ctlCapture = true;
            m_ctlCaptureTime = 0.0f;
        }
        break;
    case CE::B: case CE::Back: case CE::Guide: m_view = m_ctlReturn; break;
    default: break;
    }
}

void Menu::renderControls() {
    SDL_Renderer* r = m_renderer;
    const int w = AppConfig::kLogicalWidth;
    const SystemEntry& se = m_systems[m_ctlSys];
    const Library::Game& g = se.games[m_ctlGame];
    const auto targets = Library::buttonTargets(targetsId(se.sys->id));
    const Library::ButtonMap map = ctlMap();
    auto presets = Library::buttonPresets(se.sys->id);
    std::string preset = "Custom";
    for (const auto& p : presets)
        if (p.map == map) preset = p.name;
    const int rows = ctlRows();
    beginListClip(kListTop, kListBottom);
    for (int i = 0; i < rows; ++i) {
        float y = kListTop + i * (kRowH + kRowGap) - m_ctlScroll;
        if (y + kRowH < kListTop || y > kListBottom) continue;
        bool active = i == m_ctlSel;
        FRect row{Theme::kMargin - 16.0f, y, w - 2.0f * (Theme::kMargin - 16.0f), kRowH};
        Theme::rowCard(r, row, active);
        std::string label, value;
        bool arrows = false;
        if (i == 0) { label = "Applies to"; value = m_ctlGameScope ? "This game" : "All " + se.sys->name + " games"; arrows = true; }
        else if (i == 1) { label = "Preset"; value = preset; arrows = true; }
        else if (i == rows - 1) { label = m_ctlGameScope ? "Use the " + se.sys->name + " layout" : "Reset to default"; }
        else {
            const auto& t = targets[i - 2];
            label = t.second;
            value = m_ctlCapture && active ? "Press a button..." : map.src[t.first] == Library::Cab::None ? "-" : Library::cabLabel(map.src[t.first]);
        }
        float ty = y + (kRowH - Theme::Type::Body) * 0.5f - 4.0f;
        AppFont::draw(r, label, row.x + 24.0f, ty, Theme::Type::Body, active ? Theme::Text : Theme::TextDim);
        if (!value.empty()) {
            float vw = AppFont::measureWidth(r, value, Theme::Type::Body);
            float vx = row.x + row.w - 24.0f - vw - (active && arrows ? 30.0f : 0.0f);
            AppFont::draw(r, value, vx, ty, Theme::Type::Body, active ? Theme::accent() : Theme::Muted);
            if (active && arrows) {
                Gfx::triangle(r, {vx - 30.0f, y + kRowH * 0.5f - 9.0f, 18.0f, 18.0f}, 180.0, Theme::accent());
                Gfx::triangle(r, {row.x + row.w - 42.0f, y + kRowH * 0.5f - 9.0f, 18.0f, 18.0f}, 0.0, Theme::accent());
            }
        }
    }
    endListClip();
    drawHeader(se.sys->name + " controls", m_ctlGameScope ? g.title : "All games", 0, 0);
    Theme::footerHints(r, w, m_ctlCapture ? "Press the cabinet button to use   HOME Cancel"
                                          : "A Change   LEFT/RIGHT Choose   B Back", "");
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
    if (m_view == View::Games) m_gameScroll = scrollFor(m_gameScroll, m_gameSel, (int)m_systems[sysIndex()].shown.size(), dt);
    else if (m_view == View::Genres) m_genreScroll = scrollFor(m_genreScroll, m_genreSel, (int)genreRows(sysIndex()).size(), dt);
    else if (m_view == View::Systems) m_sysScroll = scrollFor(m_sysScroll, m_sysRow, systemRows(), dt);
    else if (m_view == View::Recent) m_recentScroll = scrollFor(m_recentScroll, m_recentSel, (int)m_recent.size(), dt);
    else if (m_view == View::Settings) m_setScroll = scrollFor(m_setScroll, m_setSel, trashRow() + 2, dt);  // + the notes
    else if (m_view == View::Bios) m_biosScroll = scrollFor(m_biosScroll, m_biosSel, (int)m_biosRows.size(), dt);
    else if (m_view == View::Controls) {
        m_ctlScroll = scrollFor(m_ctlScroll, m_ctlSel, ctlRows(), dt);
        if (m_ctlCapture && (m_ctlCaptureTime += dt) > 6.0f) m_ctlCapture = false;  // nothing pressed: give up
    }
    else m_hitScroll = scrollFor(m_hitScroll, m_hitSel, (int)m_hits.size(), dt, kHitsTop, kListBottom, kHitH + kHitGap);

    SDL_SetRenderTarget(m_renderer, m_canvas);
    SDL_RenderSetScale(m_renderer, m_canvasScale, m_canvasScale);
    Theme::backgroundAnimated(m_renderer, AppConfig::kLogicalWidth, AppConfig::kLogicalHeight, m_clock);
    if (m_view == View::Games) renderGames();
    else if (m_view == View::Search) renderSearch();
    else if (m_view == View::Recent) renderRecent();
    else if (m_view == View::Settings) renderSettings();
    else if (m_view == View::Transfer) renderTransfer();
    else if (m_view == View::Bios) renderBios();
    else if (m_view == View::Controls) renderControls();
    else if (m_view == View::Genres) renderGenres();
    else renderSystems();
    renderJump();
    if (m_popup) renderPopup();
    if (m_confirm != Confirm::None)
        Theme::confirmDialog(m_renderer, AppConfig::kLogicalWidth, AppConfig::kLogicalHeight, m_confirmQ,
                             "Cancel", m_confirmOk, m_confirmSel);
    renderToast();
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

// Reads the held directions from every controller (D-pad and left stick) and the
// keyboard, fires on the press, then repeats while held: after 400 ms, every
// 110 ms, speeding up to every 40 ms after 1.5 s.
void Menu::pollDirections(bool& running) {
    using CE = AtGames::ControlEvent;
    static const CE kEv[DirCount] = {CE::Up, CE::Down, CE::Left, CE::Right};
    static const SDL_GameControllerButton kBtn[DirCount] = {
        SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
        SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT};
    static const SDL_Scancode kKey[DirCount] = {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT};
    const int kStick = 16000;
    // A direction already held the first time a controller is seen is ignored
    // on it until released: a Bluetooth pad switched off without a clean
    // disconnect can keep reporting a held direction forever, which would
    // scroll the menu on its own (the same guard as the player's openPads).
    static std::map<SDL_JoystickID, unsigned> stuck;  // per controller, bit per direction
    bool held[DirCount] = {};
    for (int j = 0; j < SDL_NumJoysticks(); ++j) {
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(j);
        SDL_GameController* gc = SDL_GameControllerFromInstanceID(id);
        if (!gc) continue;
        bool on[DirCount] = {};
        for (int d = 0; d < DirCount; ++d)
            if (SDL_GameControllerGetButton(gc, kBtn[d])) on[d] = true;
        int x = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
        int y = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
        if (y < -kStick) on[DirUp] = true;
        if (y > kStick) on[DirDown] = true;
        if (x < -kStick) on[DirLeft] = true;
        if (x > kStick) on[DirRight] = true;
        auto seen = stuck.find(id);
        if (seen == stuck.end()) {
            unsigned m = 0;
            for (int d = 0; d < DirCount; ++d) if (on[d]) m |= 1u << d;
            if (m) log("controller %s: direction held when first seen, ignored until released", SDL_GameControllerName(gc));
            seen = stuck.emplace(id, m).first;
        }
        for (int d = 0; d < DirCount; ++d) {
            if (seen->second >> d & 1) {
                if (on[d]) continue;
                seen->second &= ~(1u << d);
            }
            if (on[d]) held[d] = true;
        }
    }
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    Uint32 now = SDL_GetTicks();
    for (int d = 0; d < DirCount; ++d) {
        if (keys && keys[kKey[d]]) held[d] = true;
        bool tap = m_dirTap[d];  // pressed and released between two polls
        m_dirTap[d] = false;
        if ((held[d] || tap) && !m_dirHeld[d]) {
            m_repeating = false;
            handle(kEv[d], running);
            m_dirSince[d] = now;
            m_dirNext[d] = now + 400;
        } else if (held[d] && m_dirHeld[d] && (Sint32)(now - m_dirNext[d]) >= 0) {
            m_repeating = true;
            handle(kEv[d], running);
            m_repeating = false;
            m_dirNext[d] = now + (now - m_dirSince[d] > 1500 ? 40 : 110);
        }
        m_dirHeld[d] = held[d];
    }
}

int Menu::run() {
    Library::ensureFolders(m_appDir);
    m_settings.load(m_appDir);
    scan();
    rebuildRows();
    // Return to where the player left off.
    for (int i = 0; i < (int)m_systems.size(); ++i)
        if (m_systems[i].sys->id == m_startSys && rowOf(i) >= 0) {
            m_sysRow = rowOf(i);
            if (!m_systems[i].games.empty()) {
                m_view = View::Games;
                // Back in the same genre, on the game (m_startIndex counts all games).
                if (hasGenres(i)) applyFilter(i, m_settings.value("genre." + m_systems[i].sys->id, ""));
                const std::vector<int>& shown = m_systems[i].shown;
                auto at = std::find(shown.begin(), shown.end(), m_startIndex);
                if (at == shown.end()) { applyFilter(i, ""); at = std::find(shown.begin(), shown.end(), m_startIndex); }
                m_gameSel = at == shown.end() ? 0 : (int)(at - shown.begin());
            }
        }
    if (!m_startSys.empty() || !m_toast.empty())
        log("menu resumes at %s #%d, message '%s'", m_startSys.c_str(), m_startIndex, m_toast.c_str());
    if (!initVideo()) return 1;
    startConsoleIcons();
    m_trackball.open();  // scrolls the lists, if there is one
    if (Updater::dailyCheckDue(m_appDir)) m_update.check(m_appDir, true);  // quietly, in the background
    for (auto& f : Library::loadFavorites(m_appDir)) m_favs.insert(f);
    if (m_startSys == "@recent" || m_startSys == "@favorites") {
        openList(m_startSys == "@favorites");
        m_sysRow = m_startSys == "@favorites" ? 2 : 1;
        if (!m_recent.empty()) {
            m_view = View::Recent;
            m_recentSel = std::max(0, std::min(m_startIndex, (int)m_recent.size() - 1));
            m_recentScroll = scrollFor(0.0f, m_recentSel, (int)m_recent.size(), 0.0f);
        }
    }
    m_sysScroll = scrollFor(0.0f, m_sysRow, systemRows(), 0.0f);
    m_gameScroll = m_view == View::Games ? scrollFor(0.0f, m_gameSel, (int)m_systems[sysIndex()].shown.size(), 0.0f) : 0.0f;

    bool running = true;
    Uint32 last = SDL_GetTicks();
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;
            using CE = AtGames::ControlEvent;
            CE ce = m_controls.event(ev, true, false);
            if (ce == CE::None) continue;
            // Directions come from pollDirections (for hold-to-repeat); the event
            // only makes sure a very short tap is not missed.
            if (ce == CE::Up) { m_dirTap[DirUp] = true; continue; }
            if (ce == CE::Down) { m_dirTap[DirDown] = true; continue; }
            if (ce == CE::Left) { m_dirTap[DirLeft] = true; continue; }
            if (ce == CE::Right) { m_dirTap[DirRight] = true; continue; }
            handle(ce, running);
        }
        pollDirections(running);
        trackballScroll(running);
        Uint32 now = SDL_GetTicks();
        float dt = std::min((now - last) / 1000.0f, 0.033f);
        last = now;
        // Updates: a new release found by the daily check, or an install finished.
        if (!m_updateAnnounced && m_update.quiet() && m_update.state() == Updater::State::Available) {
            m_updateAnnounced = true;
            m_toast = "Retro Launcher v" + m_update.latest() + " is available - Settings > Updates";
            m_toastTime = 0.0f;
        }
        if (m_update.state() == Updater::State::Done) {
            const std::string msg = m_update.message();
            log("menu: restarting after the update");
            m_toast = msg + " - restarting";
            m_toastTime = 0.0f;
            render(0.0f);
            present();
            shutdown();
            Library::execUpdated(m_appDir, msg);
            return 1;  // only if exec failed
        }
        std::string art;
        if (m_art.takeResult(art)) {
            m_toast = m_artLast = art;
            m_toastTime = 0.0f;
            m_panelKey.clear();  // the highlighted game may have art now
        }
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
