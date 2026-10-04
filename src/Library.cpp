#include "Library.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace Library {
namespace {

FILE* g_log = nullptr;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool isDir(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool isFile(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string extOf(const std::string& name) {
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? "" : lower(name.substr(dot + 1));
}

bool copyFile(const std::string& from, const std::string& to) {
    int in = ::open(from.c_str(), O_RDONLY);
    if (in < 0) return false;
    int out = ::open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out < 0) { ::close(in); return false; }
    char buf[65536];
    bool ok = true;
    for (;;) {
        ssize_t n = ::read(in, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0 || ::write(out, buf, (size_t)n) != n) { ok = false; break; }
    }
    ::close(in);
    ::close(out);
    return ok;
}

// "Sonic the Hedgehog (USA, Europe) [!]" -> title "Sonic the Hedgehog", tags "(USA, Europe) [!]"
void splitTitle(const std::string& stem, std::string& title, std::string& tags) {
    size_t cut = std::string::npos;
    for (size_t i = 0; i < stem.size(); ++i)
        if (stem[i] == '(' || stem[i] == '[') { cut = i; break; }
    title = trim(cut == std::string::npos ? stem : stem.substr(0, cut));
    tags = cut == std::string::npos ? "" : trim(stem.substr(cut));
    if (title.empty()) { title = stem; tags.clear(); }
}

void exec(const std::vector<std::string>& args) {
    // A pending alarm() survives exec and would kill the next mode with the
    // player's watchdog signal; clear it at every hand-off.
    ::alarm(0);
    std::fflush(stdout);
    if (g_log) std::fflush(g_log);
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    ::execv(selfPath().c_str(), argv.data());
    log("execv failed: %s", std::strerror(errno));
}

} // namespace

const std::string& selfPath() {
    static std::string path = [] {
        char buf[1024];
        ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n <= 0) return std::string("/proc/self/exe");
        buf[n] = 0;
        return std::string(buf);
    }();
    return path;
}

const char* screenName(ScreenId s) {
    return s == ScreenId::Playfield ? "playfield" : s == ScreenId::Dmd ? "dmd" : "backglass";
}

const char* screenLabel(ScreenId s) {
    return s == ScreenId::Playfield ? "PLAYFIELD" : s == ScreenId::Dmd ? "DMD" : "BACKGLASS";
}

bool parseScreen(const std::string& s, ScreenId& out) {
    std::string l = lower(trim(s));
    if (l == "playfield") { out = ScreenId::Playfield; return true; }
    if (l == "backglass") { out = ScreenId::Backglass; return true; }
    if (l == "dmd") { out = ScreenId::Dmd; return true; }
    return false;
}

const std::vector<System>& systems() {
    static const std::vector<System> list = {
        {"genesis", "Genesis / Mega Drive", "MD", {"genesis_plus_gx_libretro.so"},
         {"md", "gen", "smd", "bin"}, 4.0f / 3.0f, true},
        {"mastersystem", "Master System", "SMS", {"genesis_plus_gx_libretro.so"},
         {"sms"}, 4.0f / 3.0f, false},
        {"gamegear", "Game Gear", "GG", {"genesis_plus_gx_libretro.so"},
         {"gg"}, 10.0f / 9.0f, false},
        {"nes", "NES", "NES", {"fceumm_libretro.so", "nestopia_libretro.so", "quicknes_libretro.so"},
         {"nes"}, 4.0f / 3.0f, false},
        {"snes", "Super Nintendo", "SNES", {"snes9x_libretro.so", "snes_mtfaust-arm64-cortex-a53.so"},
         {"sfc", "smc"}, 4.0f / 3.0f, false},
        {"atari2600", "Atari 2600", "2600", {"stella_libretro.so", "stella2014_libretro.so"},
         {"a26", "bin"}, 4.0f / 3.0f, false},
        {"colecovision", "ColecoVision", "CV", {"gearcoleco_libretro.so", "libcv.so"},
         {"col", "rom", "bin"}, 4.0f / 3.0f, false},
        // Systems the firmware has no core for: cores shipped in the app's cores/.
        {"gb", "Game Boy", "GB", {"gambatte_libretro.so"}, {"gb"}, 10.0f / 9.0f, false},
        {"gbc", "Game Boy Color", "GBC", {"gambatte_libretro.so"}, {"gbc", "gb"}, 10.0f / 9.0f, false},
        {"gba", "Game Boy Advance", "GBA", {"gpsp_libretro.so"}, {"gba"}, 3.0f / 2.0f, false},
        {"pce", "PC Engine / TurboGrafx-16", "PCE", {"mednafen_pce_fast_libretro.so"}, {"pce", "sgx"}, 4.0f / 3.0f, false},
        {"lynx", "Atari Lynx", "LNX", {"handy_libretro.so"}, {"lnx"}, 160.0f / 102.0f, false},
    };
    return list;
}

std::vector<std::pair<std::string, std::string>> controlHints(const std::string& id) {
    if (id == "genesis")
        return {{"A", "B"}, {"B", "C"}, {"X", "A"}, {"START", "Start"}};
    if (id == "mastersystem" || id == "gamegear")
        return {{"A", "Button 1"}, {"B", "Button 2"}, {"START", id == "gamegear" ? "Start" : "Pause"}};
    if (id == "nes")
        return {{"A", "B"}, {"B", "A"}, {"START", "Start"}, {"SELECT", "Select"}};
    if (id == "snes")
        return {{"A", "B"}, {"B", "A"}, {"X", "Y"}, {"Y", "X"}, {"LB / RB", "L / R"}, {"START", "Start"}};
    if (id == "atari2600")
        return {{"A", "Fire"}, {"START", "Reset"}, {"SELECT", "Select"}};
    if (id == "colecovision")
        return {{"A", "Left fire"}, {"B", "Right fire"}, {"START", "Keypad *"}};
    if (id == "gb" || id == "gbc")
        return {{"A", "B"}, {"B", "A"}, {"START", "Start"}, {"SELECT", "Select"}};
    if (id == "gba")
        return {{"A", "B"}, {"B", "A"}, {"LB / RB", "L / R"}, {"START", "Start"}, {"SELECT", "Select"}};
    if (id == "pce")
        return {{"A", "II"}, {"B", "I"}, {"START", "Run"}, {"SELECT", "Select"}};
    if (id == "lynx")
        return {{"A", "B"}, {"B", "A"}, {"LB / RB", "Option 1 / 2"}, {"START", "Pause"}};
    return {};
}

const System* findSystem(const std::string& id) {
    for (const System& s : systems())
        if (s.id == id) return &s;
    return nullptr;
}

std::vector<Game> scanGames(const std::string& appDir, const System& sys) {
    std::vector<Game> games;
    std::string dir = appDir + "/roms/" + sys.id;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return games;
    while (struct dirent* e = ::readdir(d)) {
        std::string name = e->d_name;
        if (name.empty() || name[0] == '.') continue;
        std::string full = dir + "/" + name;
        if (!isFile(full)) continue;
        std::string ext = extOf(name);
        if (ext != "zip" && std::find(sys.extensions.begin(), sys.extensions.end(), ext) == sys.extensions.end())
            continue;
        Game g;
        g.file = name;
        g.path = full;
        splitTitle(name.substr(0, name.size() - ext.size() - 1), g.title, g.tags);
        games.push_back(g);
    }
    ::closedir(d);
    std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) {
        return lower(a.title + " " + a.tags) < lower(b.title + " " + b.tags);
    });
    return games;
}

void ensureFolders(const std::string& appDir) {
    for (const char* sub : {"/roms", "/saves", "/system", "/cores", "/data"})
        ::mkdir((appDir + sub).c_str(), 0755);
    for (const System& s : systems()) {
        ::mkdir((appDir + "/roms/" + s.id).c_str(), 0755);
        ::mkdir((appDir + "/saves/" + s.id).c_str(), 0755);
    }
}

// ------------------------------------------------------------------ settings

void Settings::load(const std::string& appDir) {
    m_path = appDir + "/data/settings.cfg";
    m_values.clear();
    std::ifstream in(m_path);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq != std::string::npos) m_values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
}

bool Settings::save() const {
    FILE* f = std::fopen(m_path.c_str(), "w");
    if (!f) { log("could not save %s: %s", m_path.c_str(), std::strerror(errno)); return false; }
    std::fprintf(f, "# Retro Launcher settings. sys.<system> = default screen, game.<system>/<file> = per-game screen,\n"
                    "# rotate.<screen> = picture rotation in degrees (0/90/180/270).\n"
                    "# bars = ambient (glow + bokeh from the game's colours) or black.\n");
    for (const auto& kv : m_values) std::fprintf(f, "%s = %s\n", kv.first.c_str(), kv.second.c_str());
    std::fclose(f);
    return true;
}

ScreenId Settings::systemScreen(const std::string& sys) const {
    ScreenId s = ScreenId::Backglass;  // the screen proven on hardware
    auto def = m_values.find("screen.default");  // Settings > Default game screen
    if (def != m_values.end()) parseScreen(def->second, s);
    auto it = m_values.find("sys." + sys);
    if (it != m_values.end()) parseScreen(it->second, s);
    return s;
}

void Settings::setSystemScreen(const std::string& sys, ScreenId s) { m_values["sys." + sys] = screenName(s); }

bool Settings::gameScreen(const std::string& sys, const std::string& file, ScreenId& out) const {
    auto it = m_values.find("game." + sys + "/" + file);
    return it != m_values.end() && parseScreen(it->second, out);
}

void Settings::setGameScreen(const std::string& sys, const std::string& file, ScreenId s) {
    m_values["game." + sys + "/" + file] = screenName(s);
}

void Settings::clearGameScreen(const std::string& sys, const std::string& file) {
    m_values.erase("game." + sys + "/" + file);
}

ScreenId Settings::screenFor(const std::string& sys, const std::string& file) const {
    ScreenId s;
    return gameScreen(sys, file, s) ? s : systemScreen(sys);
}

void Settings::set(const std::string& key, const std::string& value) { m_values[key] = value; }

std::string Settings::value(const std::string& key, const std::string& fallback) const {
    auto it = m_values.find(key);
    return it == m_values.end() ? fallback : it->second;
}

int Settings::rotation(ScreenId s, int fallback) const {
    auto it = m_values.find(std::string("rotate.") + screenName(s));
    return it == m_values.end() ? fallback : std::atoi(it->second.c_str());
}

// ------------------------------------------------------------------ logging

void openLog(const std::string& appDir, const char* mode) {
    std::string path = appDir + "/data/launcher.log";
    struct stat st;
    bool big = ::stat(path.c_str(), &st) == 0 && st.st_size > 512 * 1024;
    g_log = std::fopen(path.c_str(), big ? "w" : "a");
    log("==== retro-launcher %s mode, pid %d", mode, (int)::getpid());
}

int logFd() { return g_log ? fileno(g_log) : -1; }

void log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fprintf(stdout, "[launcher] %s\n", buf);
    std::fflush(stdout);
    if (g_log) { std::fprintf(g_log, "%s\n", buf); std::fflush(g_log); }
}

// ------------------------------------------------------------------- cores

std::string findCore(const std::string& appDir, const System& sys, std::string& where) {
    // 1. Our own cores (copied off the no-exec stick before loading).
    for (const std::string& c : sys.cores) {
        std::string src = appDir + "/cores/" + c;
        if (!isFile(src)) continue;
        ::mkdir("/tmp/retrofe/cores", 0755);
        std::string dst = "/tmp/retrofe/cores/" + c;
        if (copyFile(src, dst)) { where = "app cores/"; return dst; }
        log("could not copy %s to %s: %s", src.c_str(), dst.c_str(), std::strerror(errno));
    }
    // 2. The firmware's core folders, in the order tableDB_retroplayer.sh uses.
    static const char* kDirs[] = {"/upgrade/opt/retroplayer/core", "/upgrade/retroplayer/core",
                                  "/app/retroplayer/core", "/emulator"};
    for (const char* dir : kDirs) {
        if (!isDir(dir)) continue;
        for (const std::string& c : sys.cores) {
            std::string p = std::string(dir) + "/" + c;
            if (::access(p.c_str(), R_OK) == 0) { where = std::string("firmware ") + dir; return p; }
        }
    }
    where = "none";
    return "";
}

// ---------------------------------------------------------------- hand-off

void execMenu(const std::string& appDir, const std::string& sys, int index, const std::string& message) {
    log("-> menu (%s #%d) %s", sys.c_str(), index, message.c_str());
    exec({"retro-launcher", "--menu", appDir, sys, std::to_string(index), message});
}

void execPlay(const std::string& appDir, const std::string& sys, const std::string& romPath, ScreenId screen,
              int index, const std::string& returnTo) {
    log("-> play %s on %s", romPath.c_str(), screenName(screen));
    exec({"retro-launcher", "--play", appDir, sys, romPath, screenName(screen), std::to_string(index),
          returnTo.empty() ? sys : returnTo});
}

namespace {
std::vector<std::pair<std::string, std::string>> loadPairs(const std::string& path) {
    std::vector<std::pair<std::string, std::string>> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) continue;
        out.push_back({line.substr(0, tab), line.substr(tab + 1)});
    }
    return out;
}

void savePairs(const std::string& path, const std::vector<std::pair<std::string, std::string>>& list) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) { log("could not save %s: %s", path.c_str(), std::strerror(errno)); return; }
    for (auto& e : list) std::fprintf(f, "%s\t%s\n", e.first.c_str(), e.second.c_str());
    std::fclose(f);
}
} // namespace

std::vector<std::pair<std::string, std::string>> loadRecent(const std::string& appDir) {
    return loadPairs(appDir + "/data/recent.txt");
}

std::vector<std::pair<std::string, std::string>> loadFavorites(const std::string& appDir) {
    return loadPairs(appDir + "/data/favorites.txt");
}

void saveFavorites(const std::string& appDir, const std::vector<std::pair<std::string, std::string>>& list) {
    savePairs(appDir + "/data/favorites.txt", list);
}

void pushRecent(const std::string& appDir, const std::string& sys, const std::string& file) {
    auto list = loadRecent(appDir);
    list.erase(std::remove(list.begin(), list.end(), std::make_pair(sys, file)), list.end());
    list.insert(list.begin(), {sys, file});
    if (list.size() > 20) list.resize(20);
    savePairs(appDir + "/data/recent.txt", list);
}

} // namespace Library
