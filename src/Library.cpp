#include "Library.h"
#include "Arcade.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

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

void exec(const std::vector<std::string>& args, const std::string& program = "") {
    // A pending alarm() survives exec and would kill the next mode with the
    // player's watchdog signal; clear it at every hand-off.
    ::alarm(0);
    std::fflush(stdout);
    if (g_log) std::fflush(g_log);
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const std::string& path = program.empty() ? selfPath() : program;
    ::execv(path.c_str(), argv.data());
    log("execv %s failed: %s", path.c_str(), std::strerror(errno));
}

} // namespace

void logInputDevices() {
    std::ifstream in("/proc/bus/input/devices");
    std::string line, name;
    while (std::getline(in, line)) {
        if (line.compare(0, 9, "N: Name=\"") == 0) name = line.substr(9, line.size() - 10);
        else if (line.compare(0, 12, "H: Handlers=") == 0) log("input: %s [%s]", name.c_str(), line.substr(12).c_str());
    }
}

const std::string& selfPath() {
    static std::string path = [] {
        char buf[1024];
        ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n <= 0) return std::string("/proc/self/exe");
        buf[n] = 0;
        // Once the file has been replaced (Settings > Updates), the kernel adds
        // " (deleted)" to the old one's name; the path itself is still right.
        std::string p(buf);
        const std::string gone = " (deleted)";
        if (p.size() > gone.size() && p.compare(p.size() - gone.size(), gone.size(), gone) == 0) p.resize(p.size() - gone.size());
        return p;
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
        // Arcade: zips are matched to a core per game (Arcade.cpp); FBNeo first.
        {"arcade", "Arcade", "ARC", {"fbneo_libretro.so", "mame2003_plus_libretro.so"}, {"zip"}, 4.0f / 3.0f, true},
        // Flycast (GPU), checked against its own NAOMI / Atomiswave list.
        {"naomi", "NAOMI", "NAO", {"flycast_libretro.so"}, {"zip"}, 4.0f / 3.0f, false},
        {"atomiswave", "Atomiswave", "AW", {"flycast_libretro.so"}, {"zip"}, 4.0f / 3.0f, false},
        // Neo Geo MVS / AES sets, FBNeo, checked per set like the arcade ones.
        {"neogeo", "Neo Geo", "NEO", {"fbneo_libretro.so"}, {"zip"}, 4.0f / 3.0f, false},
        {"genesis", "Genesis / Mega Drive", "MD", {"genesis_plus_gx_libretro.so"},
         {"md", "gen", "smd", "bin"}, 4.0f / 3.0f, true},
        {"mastersystem", "Master System", "SMS", {"genesis_plus_gx_libretro.so"},
         {"sms"}, 4.0f / 3.0f, false},
        {"gamegear", "Game Gear", "GG", {"genesis_plus_gx_libretro.so"},
         {"gg"}, 10.0f / 9.0f, false},
        {"nes", "NES", "NES", {"fceumm_libretro.so", "nestopia_libretro.so", "quicknes_libretro.so"},
         {"nes"}, 4.0f / 3.0f, true},
        {"snes", "Super Nintendo", "SNES", {"snes9x_libretro.so", "snes_mtfaust-arm64-cortex-a53.so"},
         {"sfc", "smc"}, 4.0f / 3.0f, true},
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
        // Disc images are passed to the core where they are (cue tracks beside them).
        // GPU core (OpenGL ES): Flycast.
        {"dreamcast", "Dreamcast", "DC", {"flycast_libretro.so"}, {"chd", "cdi", "gdi", "cue", "m3u"}, 4.0f / 3.0f, true},
        {"n64", "Nintendo 64", "N64", {"mupen64plus_next_libretro.so"}, {"n64", "z64", "v64"}, 4.0f / 3.0f, false},
        {"saturn", "Saturn", "SAT", {"yabasanshiro_libretro.so"}, {"chd", "cue", "iso", "ccd"}, 4.0f / 3.0f, false},
        {"psp", "PSP", "PSP", {"ppsspp_libretro.so"}, {"iso", "cso", "chd", "pbp", "elf"}, 16.0f / 9.0f, false},
        {"psx", "PlayStation", "PS1", {"pcsx_rearmed_libretro.so"}, {"chd", "cue", "pbp", "m3u", "iso", "img", "bin"},
         4.0f / 3.0f, true},
    };
    return list;
}

// ------------------------------------------------------------ buttons

namespace {
enum Pad { kB = 0, kY = 1, kSelect = 2, kStart = 3, kA = 8, kX = 9, kL = 10, kR = 11, kL2 = 12, kR2 = 13, kL3 = 14, kR3 = 15 };
const char* const kPadNames[16] = {"B", "Y", "SELECT", "START", "", "", "", "", "A", "X", "L", "R", "L2", "R2", "L3", "R3"};
const char* const kCabNames[] = {"", "A", "B", "X", "Y", "LB", "RB", "LB2", "RB2", "START", "REWIND", "REWIND2"};
}

const char* cabLabel(Cab c) { return (int)c < (int)Cab::Count ? kCabNames[(int)c] : ""; }

bool ButtonMap::operator==(const ButtonMap& o) const {
    for (int i = 0; i < 16; ++i)
        if (src[i] != o.src[i]) return false;
    return true;
}

ButtonMap defaultButtonMap() {
    ButtonMap m;
    for (Cab& c : m.src) c = Cab::None;
    // Cabinet A/B/X/Y -> RetroPad B/A/Y/X (SNES-style; Genesis A/B/X = B/C/A).
    m.src[kB] = Cab::A; m.src[kA] = Cab::B; m.src[kY] = Cab::X; m.src[kX] = Cab::Y;
    m.src[kL] = Cab::LB; m.src[kR] = Cab::RB; m.src[kL2] = Cab::LB2; m.src[kR2] = Cab::RB2;
    m.src[kStart] = Cab::Start; m.src[kSelect] = Cab::Rewind;  // the cabinet has no Select
    return m;
}

std::string buttonMapToString(const ButtonMap& m) {
    std::string out;
    for (int i = 0; i < 16; ++i)
        if (kPadNames[i][0] && m.src[i] != Cab::None)
            out += std::string(out.empty() ? "" : ",") + kPadNames[i] + ":" + cabLabel(m.src[i]);
    return out;
}

bool buttonMapFromString(const std::string& str, ButtonMap& out) {
    if (trim(str).empty()) return false;
    ButtonMap m;
    for (Cab& c : m.src) c = Cab::None;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, ',')) {
        size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        std::string pad = trim(item.substr(0, colon)), cab = trim(item.substr(colon + 1));
        int id = -1;
        for (int i = 0; i < 16; ++i)
            if (kPadNames[i][0] && pad == kPadNames[i]) id = i;
        for (int c = 1; c < (int)Cab::Count; ++c)
            if (id >= 0 && cab == kCabNames[c]) m.src[id] = (Cab)c;
    }
    out = m;
    return true;
}

std::vector<std::pair<int, std::string>> buttonTargets(const std::string& id) {
    if (id == "genesis")
        return {{kY, "A"}, {kB, "B"}, {kA, "C"}, {kL, "X"}, {kX, "Y"}, {kR, "Z"}, {kStart, "Start"}, {kSelect, "Mode"}};
    if (id == "mastersystem" || id == "gamegear")
        return {{kB, "Button 1"}, {kA, "Button 2"}, {kStart, id == "gamegear" ? "Start" : "Pause"}};
    if (id == "nes")
        return {{kB, "B"}, {kA, "A"}, {kStart, "Start"}, {kSelect, "Select"}};
    if (id == "snes")
        return {{kB, "B"}, {kA, "A"}, {kY, "Y"}, {kX, "X"}, {kL, "L"}, {kR, "R"}, {kStart, "Start"}, {kSelect, "Select"}};
    if (id == "atari2600")
        return {{kB, "Fire"}, {kStart, "Reset"}, {kSelect, "Select"}};
    if (id == "colecovision:libcv")  // the firmware core (no BIOS file); see Player's inputState
        return {{kA, "Left fire"}, {kB, "Right fire"}, {kStart, "Keypad 1"}, {kSelect, "Keypad *"},
                {kX, "Keypad (on screen)"}};
    if (id == "colecovision")
        return {{kB, "Left fire"}, {kA, "Right fire"}, {kStart, "Keypad *"}, {kSelect, "Keypad #"}};
    if (id == "gb" || id == "gbc")
        return {{kB, "B"}, {kA, "A"}, {kStart, "Start"}, {kSelect, "Select"}};
    if (id == "gba")
        return {{kB, "B"}, {kA, "A"}, {kL, "L"}, {kR, "R"}, {kStart, "Start"}, {kSelect, "Select"}};
    if (id == "pce")
        return {{kB, "II"}, {kA, "I"}, {kStart, "Run"}, {kSelect, "Select"}};
    if (id == "lynx")
        return {{kB, "B"}, {kA, "A"}, {kL, "Option 1"}, {kR, "Option 2"}, {kStart, "Pause"}};
    if (id == "psx")
        return {{kB, "Cross"}, {kA, "Circle"}, {kY, "Square"}, {kX, "Triangle"}, {kL, "L1"}, {kR, "R1"},
                {kL2, "L2"}, {kR2, "R2"}, {kStart, "Start"}, {kSelect, "Select"}};
    if (id == "psp")
        return {{kB, "Cross"}, {kA, "Circle"}, {kY, "Square"}, {kX, "Triangle"}, {kL, "L"}, {kR, "R"},
                {kStart, "Start"}, {kSelect, "Select"}};
    if (id == "n64")  // with the core's independent C-button layout (set as the default)
        return {{kB, "A"}, {kY, "B"}, {kL2, "Z"}, {kSelect, "L"}, {kR2, "R"}, {kX, "C-Up"}, {kA, "C-Down"},
                {kL, "C-Left"}, {kR, "C-Right"}, {kStart, "Start"}};
    if (id == "saturn")
        return {{kB, "A"}, {kA, "B"}, {kL, "C"}, {kY, "X"}, {kX, "Y"}, {kR, "Z"}, {kL2, "L"}, {kR2, "R"}, {kStart, "Start"}};
    if (id == "dreamcast")
        return {{kB, "A"}, {kA, "B"}, {kY, "X"}, {kX, "Y"}, {kL2, "L trigger"}, {kR2, "R trigger"}, {kStart, "Start"}};
    if (id == "neogeo")
        return {{kB, "A"}, {kA, "B"}, {kY, "C"}, {kX, "D"}, {kSelect, "Coin"}, {kStart, "Start"}};
    if (id == "arcade" || id == "naomi" || id == "atomiswave")
        return {{kB, "Button 1"}, {kA, "Button 2"}, {kY, "Button 3"}, {kX, "Button 4"}, {kL, "Button 5"},
                {kR, "Button 6"}, {kSelect, "Coin"}, {kStart, "Start"}};
    return {{kB, "B"}, {kA, "A"}, {kStart, "Start"}, {kSelect, "Select"}};
}

std::string buttonMapKey(const std::string& sys, const std::string& file) {
    return "controls." + sys + (file.empty() ? "" : "/" + file);
}

ButtonMap buttonMapFor(const Settings& s, const std::string& sys, const std::string& file, int* scope) {
    ButtonMap m;
    if (!file.empty() && buttonMapFromString(s.value(buttonMapKey(sys, file), ""), m)) { if (scope) *scope = 2; return m; }
    if (buttonMapFromString(s.value(buttonMapKey(sys, ""), ""), m)) { if (scope) *scope = 1; return m; }
    if (scope) *scope = 0;
    return defaultButtonMap();
}

std::vector<ButtonPreset> buttonPresets(const std::string& sys) {
    (void)sys;
    ButtonMap def = defaultButtonMap();
    auto remap = [&def](std::initializer_list<std::pair<Cab, Cab>> moves) {
        ButtonMap m = def;
        for (Cab& c : m.src)
            for (const auto& mv : moves)
                if (c == mv.first) { c = mv.second; break; }
        return m;
    };
    return {
        {"Default", def},
        {"Swap A and B", remap({{Cab::A, Cab::B}, {Cab::B, Cab::A}})},
        // A pinball cabinet's flippers are its best buttons: the two main
        // actions there (fire / jump), A and B taking over the flippers' jobs.
        {"Flippers as A and B", remap({{Cab::A, Cab::LB}, {Cab::B, Cab::RB}, {Cab::LB, Cab::A}, {Cab::RB, Cab::B}})},
    };
}

std::vector<std::pair<std::string, std::string>> controlHints(const std::string& id, const ButtonMap& m) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& t : buttonTargets(id))
        if (m.src[t.first] != Cab::None) out.push_back({cabLabel(m.src[t.first]), t.second});
    return out;
}

std::vector<std::pair<std::string, std::string>> controlHints(const std::string& id) {
    return controlHints(id, defaultButtonMap());
}

const System* findSystem(const std::string& id) {
    for (const System& s : systems())
        if (s.id == id) return &s;
    return nullptr;
}

// ------------------------------------------------------------------ trash

namespace {
// Files a disc sheet or playlist names, as written (relative to its folder).
std::vector<std::string> sheetRefs(const std::string& path) {
    std::vector<std::string> refs;
    std::string ext = extOf(path);
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string ref;
        if (ext == "cue") {
            size_t a = line.find('"'), b = line.rfind('"');
            if (lower(line).find("file") == std::string::npos || a == std::string::npos || b <= a) continue;
            ref = line.substr(a + 1, b - a - 1);
        } else if (ext == "gdi") {
            size_t q = line.find('"');
            if (q != std::string::npos) {
                size_t e = line.find('"', q + 1);
                if (e == std::string::npos) continue;
                ref = line.substr(q + 1, e - q - 1);
            } else {
                std::istringstream fields(line);
                std::string f[5];
                if (!(fields >> f[0] >> f[1] >> f[2] >> f[3] >> f[4])) continue;
                ref = f[4];
            }
        } else if (ext == "m3u") {
            ref = trim(line);
            if (ref.empty() || ref[0] == '#') continue;
        }
        if (!ref.empty() && ref.find("..") == std::string::npos && ref[0] != '/') refs.push_back(ref);
    }
    return refs;
}

bool makeDirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/') {
            std::string part = path.substr(0, i);
            if (!isDir(part) && ::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    return true;
}

unsigned long long sizeOf(const std::string& path) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return (unsigned long long)st.st_size;
    unsigned long long total = 0;
    if (DIR* d = ::opendir(path.c_str())) {
        while (struct dirent* e = ::readdir(d)) {
            std::string n = e->d_name;
            if (n != "." && n != "..") total += sizeOf(path + "/" + n);
        }
        ::closedir(d);
    }
    return total;
}

bool removeAll(const std::string& path) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) return true;
    if (S_ISDIR(st.st_mode)) {
        if (DIR* d = ::opendir(path.c_str())) {
            while (struct dirent* e = ::readdir(d)) {
                std::string n = e->d_name;
                if (n != "." && n != "..") removeAll(path + "/" + n);
            }
            ::closedir(d);
        }
        return ::rmdir(path.c_str()) == 0;
    }
    return ::unlink(path.c_str()) == 0;
}
} // namespace

std::vector<std::string> gameParts(const std::string& appDir, const std::string& sys, const Game& g) {
    const std::string dir = appDir + "/roms/" + sys + "/";
    std::vector<std::string> parts;
    size_t slash = g.file.find('/');
    if (slash != std::string::npos) return {g.file.substr(0, slash)};  // a game in its own folder
    parts.push_back(g.file);
    std::string ext = extOf(g.file);
    if (ext == "cue" || ext == "gdi" || ext == "m3u")
        for (const std::string& ref : sheetRefs(dir + g.file)) {
            if (isFile(dir + ref)) parts.push_back(ref);
            std::string rext = extOf(ref);
            if (rext == "cue" || rext == "gdi")  // an .m3u's discs bring their own tracks
                for (const std::string& r2 : sheetRefs(dir + ref))
                    if (isFile(dir + r2)) parts.push_back(r2);
        }
    if (g.arcade) {  // a NAOMI GD-ROM folder beside the zip
        std::string set = g.file.substr(0, g.file.size() - 4);
        if (isDir(dir + set)) parts.push_back(set);
    }
    std::sort(parts.begin(), parts.end());
    parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
    return parts;
}

bool moveToTrash(const std::string& appDir, const std::string& sys, const std::vector<std::string>& parts,
                 std::string& error) {
    for (const std::string& rel : parts) {
        std::string from = appDir + "/roms/" + sys + "/" + rel;
        std::string to = appDir + "/trash/" + sys + "/" + rel;
        size_t slash = to.find_last_of('/');
        if (!makeDirs(to.substr(0, slash))) { error = "could not create " + to.substr(0, slash); return false; }
        for (int n = 2; ::access(to.c_str(), F_OK) == 0; ++n) {  // already one in the trash: keep both
            size_t dot = rel.find_last_of('.');
            std::string base = dot == std::string::npos || isDir(from) ? rel : rel.substr(0, dot);
            std::string tail = dot == std::string::npos || isDir(from) ? "" : rel.substr(dot);
            to = appDir + "/trash/" + sys + "/" + base + " (" + std::to_string(n) + ")" + tail;
        }
        if (::rename(from.c_str(), to.c_str()) != 0) { error = rel + ": " + std::strerror(errno); return false; }
        log("trash: %s/%s -> %s", sys.c_str(), rel.c_str(), to.c_str());
    }
    return true;
}

unsigned long long trashSize(const std::string& appDir) { return sizeOf(appDir + "/trash"); }

bool emptyTrash(const std::string& appDir) {
    bool ok = removeAll(appDir + "/trash");
    log("trash emptied: %s", ok ? "ok" : "some files could not be removed");
    return ok;
}

bool isArcadeSystem(const std::string& id) {
    return id == "arcade" || id == "naomi" || id == "atomiswave" || id == "neogeo";
}

// The disc number in a name like "Final Fantasy VII (USA) (Disc 2)" or
// "(Disc 2 of 3)" / "(CD2)", and the name without it; 0 if there is none.
int discNumber(const std::string& stem, std::string& base) {
    const std::string l = lower(stem);
    for (const char* tag : {"(disc ", "(disk ", "(cd"}) {
        size_t at = l.find(tag);
        if (at == std::string::npos) continue;
        size_t p = at + std::strlen(tag);
        while (p < l.size() && l[p] == ' ') ++p;
        if (p >= l.size() || !std::isdigit((unsigned char)l[p])) continue;
        int n = std::atoi(l.c_str() + p);
        size_t close = l.find(')', p);
        if (close == std::string::npos) continue;
        base = stem.substr(0, at) + stem.substr(close + 1);
        // Tidy the spaces left where the tag was.
        std::string tidy;
        for (char c : base)
            if (!(c == ' ' && (tidy.empty() || tidy.back() == ' '))) tidy += c;
        while (!tidy.empty() && tidy.back() == ' ') tidy.pop_back();
        base = tidy;
        return n;
    }
    return 0;
}

// The player names a game's saves after its playlist once it has one: carry the
// newest per-disc save (named after each disc's file or folder) over to it.
void carryDiscSave(const std::string& appDir, const System& sys, const std::string& game, const std::vector<std::string>& discStems) {
    const std::string saves = appDir + "/saves/" + sys.id + "/";
    const std::string target = saves + game + ".srm";
    if (isFile(target)) return;
    // A PlayStation memory card that holds saves: its directory (frames 1-15
    // of block 0, 128 bytes each) has a block in use (0x51). Starting another
    // disc on its own writes a blank card, which must not win over real saves.
    auto hasSaves = [&](const std::string& path) {
        if (sys.id != "psx") return true;
        std::ifstream in(path, std::ios::binary);
        std::vector<char> head(0x800);
        if (!in.read(head.data(), (std::streamsize)head.size())) return false;
        for (int frame = 1; frame <= 15; ++frame)
            if ((unsigned char)head[frame * 0x80] == 0x51) return true;
        return false;
    };
    std::string newest, newestAny;
    time_t newestTime = 0, newestAnyTime = 0;
    for (const std::string& stem : discStems) {
        std::string srm = saves + stem + ".srm";
        struct stat st;
        if (::stat(srm.c_str(), &st) != 0) continue;
        if (st.st_mtime >= newestAnyTime) { newestAny = srm; newestAnyTime = st.st_mtime; }
        if (st.st_mtime >= newestTime && hasSaves(srm)) { newest = srm; newestTime = st.st_mtime; }
    }
    if (newest.empty()) newest = newestAny;  // only blank cards: any of them
    if (!newest.empty() && copyFile(newest, target)) log("%s: save %s carried over", sys.id.c_str(), newest.substr(newest.find_last_of('/') + 1).c_str());
}

// Multi-disc games kept one disc per folder ("Game (Disc 1)/", "Game (Disc 2)/"...,
// how many downloads unpack): one playlist beside the folders, pointing into
// each, so they too become one game.
// `folderName`: `dir` is a game's own folder holding its disc folders
// ("Game/Game (Disc 1)/..."): the playlist goes inside, named after it.
void makeFolderDiscPlaylists(const std::string& appDir, const System& sys, const std::string& dir,
                             const std::string& folderName = "") {
    static const std::set<std::string> discExts = {"chd", "cue", "pbp", "iso", "cdi", "gdi", "ccd", "img"};
    std::map<std::string, std::map<int, std::string>> groups;  // base name -> disc -> "folder/file"
    std::set<std::string> unclear;                              // a disc folder with no single disc file
    if (DIR* d = ::opendir(dir.c_str())) {
        while (struct dirent* e = ::readdir(d)) {
            std::string folder = e->d_name;
            if (folder.empty() || folder[0] == '.' || !isDir(dir + "/" + folder)) continue;
            std::string base;
            int n = discNumber(folder, base);
            if (n <= 0) continue;
            // The folder's disc: its one sheet (.cue / .gdi / .ccd), else its one image.
            // Unzipping often leaves the files one folder further in
            // ("Game (Disc 3)/Game (Disc 3)/..."): then that folder's.
            auto findDisc = [&](const std::string& path, std::string& onlySub) {
                std::vector<std::string> sheets, images, subs;
                if (DIR* sd = ::opendir(path.c_str())) {
                    while (struct dirent* f = ::readdir(sd)) {
                        std::string name = f->d_name;
                        if (name.empty() || name[0] == '.') continue;
                        if (isDir(path + "/" + name)) { subs.push_back(name); continue; }
                        std::string ext = extOf(name);
                        if (!discExts.count(ext) || std::find(sys.extensions.begin(), sys.extensions.end(), ext) == sys.extensions.end())
                            continue;
                        (ext == "cue" || ext == "gdi" || ext == "ccd" ? sheets : images).push_back(name);
                    }
                    ::closedir(sd);
                }
                onlySub = subs.size() == 1 ? subs[0] : "";
                return sheets.size() == 1 ? sheets[0] : sheets.empty() && images.size() == 1 ? images[0] : std::string();
            };
            std::string inner, ignored;
            std::string disc = findDisc(dir + "/" + folder, inner);
            if (disc.empty() && !inner.empty()) {
                std::string deeper = findDisc(dir + "/" + folder + "/" + inner, ignored);
                if (!deeper.empty()) disc = inner + "/" + deeper;
            }
            if (disc.empty()) {
                log("%s: %s: no single disc file (.cue/.chd...) found, so its game isn't grouped", sys.id.c_str(), folder.c_str());
                unclear.insert(base);
                continue;
            }
            groups[base][n] = folder + "/" + disc;
        }
        ::closedir(d);
    }
    if (!folderName.empty()) {
        // Inside a game's folder: one game only, and only if it has no playlist yet.
        if (groups.size() != 1) return;
        if (DIR* d = ::opendir(dir.c_str())) {
            bool has = false;
            while (struct dirent* e = ::readdir(d)) has = has || extOf(e->d_name) == "m3u";
            ::closedir(d);
            if (has) return;
        }
    }
    for (const auto& g : groups) {
        if (g.second.size() < 2 || unclear.count(g.first)) continue;
        const std::string game = folderName.empty() ? g.first : folderName;
        const std::string m3u = dir + "/" + game + ".m3u";
        if (isFile(m3u)) continue;
        std::ofstream out(m3u);
        for (const auto& disc : g.second) out << disc.second << "\n";
        out.close();
        if (!out) { log("%s: could not write %s", sys.id.c_str(), m3u.c_str()); continue; }
        log("%s: made %s%s.m3u for %zu disc folders", sys.id.c_str(), folderName.empty() ? "" : (folderName + "/").c_str(),
            game.c_str(), g.second.size());
        std::vector<std::string> stems;  // a folder game's saves are named after its folder
        for (const auto& disc : g.second) stems.push_back(disc.second.substr(0, disc.second.find('/')));
        carryDiscSave(appDir, sys, game, stems);
    }
}

// Multi-disc games (PlayStation, Dreamcast): discs named "... (Disc 1)", "(Disc 2)"... in one folder
// with no playlist get one (<game>.m3u beside them), so the menu lists one game
// with disc swapping (pause menu > Change disc) and one memory card. The newest
// per-disc save becomes the game's, if it has none yet. `folderName`: a game in
// a folder of its own, whose playlist is named after the folder.
void makeDiscPlaylists(const std::string& appDir, const System& sys, const std::string& dir, const std::string& folderName) {
    static const std::set<std::string> discExts = {"chd", "cue", "pbp", "iso", "cdi", "gdi", "ccd", "img"};
    std::map<std::string, std::map<int, std::string>> groups;  // base name -> disc -> file
    bool hasPlaylist = false;
    if (DIR* d = ::opendir(dir.c_str())) {
        while (struct dirent* e = ::readdir(d)) {
            std::string name = e->d_name;
            if (name.empty() || name[0] == '.') continue;
            std::string ext = extOf(name);
            if (ext == "m3u") hasPlaylist = true;
            if (!discExts.count(ext) || std::find(sys.extensions.begin(), sys.extensions.end(), ext) == sys.extensions.end())
                continue;
            std::string base;
            int n = discNumber(name.substr(0, name.size() - ext.size() - 1), base);
            if (n > 0) groups[folderName.empty() ? base : folderName][n] = name;
        }
        ::closedir(d);
    }
    if (!folderName.empty() && hasPlaylist) return;  // the folder already has its playlist
    for (const auto& g : groups) {
        if (g.second.size() < 2) continue;
        const std::string m3u = dir + "/" + g.first + ".m3u";
        if (isFile(m3u)) continue;
        std::ofstream out(m3u);
        for (const auto& disc : g.second) out << disc.second << "\n";
        out.close();
        if (!out) { log("%s: could not write %s", sys.id.c_str(), m3u.c_str()); continue; }
        log("%s: made %s.m3u for %zu discs", sys.id.c_str(), g.first.c_str(), g.second.size());
        std::vector<std::string> stems;
        for (const auto& disc : g.second) stems.push_back(disc.second.substr(0, disc.second.find_last_of('.')));
        carryDiscSave(appDir, sys, g.first, stems);
    }
}

std::vector<Game> scanGames(const std::string& appDir, const System& sys) {
    if (isArcadeSystem(sys.id)) return Arcade::scan(appDir, sys);
    std::vector<Game> games;
    std::string dir = appDir + "/roms/" + sys.id;
    // Only where the core reads playlists and swaps discs: PCSX ReARMed and
    // Flycast. YabaSanshiro (Saturn) has neither, so its discs stay separate.
    if (sys.id == "psx" || sys.id == "dreamcast") {
        makeDiscPlaylists(appDir, sys, dir, "");
        makeFolderDiscPlaylists(appDir, sys, dir);
        if (DIR* d = ::opendir(dir.c_str())) {  // and in games' own folders
            std::vector<std::string> subs;
            while (struct dirent* e = ::readdir(d)) {
                std::string n = e->d_name;
                if (!n.empty() && n[0] != '.' && isDir(dir + "/" + n)) subs.push_back(n);
            }
            ::closedir(d);
            for (const std::string& sub : subs) {
                makeDiscPlaylists(appDir, sys, dir + "/" + sub, sub);
                makeFolderDiscPlaylists(appDir, sys, dir + "/" + sub, sub);  // "Game/Game (Disc 1)/..."
            }
        }
    }
    DIR* d = ::opendir(dir.c_str());
    if (!d) return games;
    std::vector<std::string> subdirs;
    while (struct dirent* e = ::readdir(d)) {
        std::string name = e->d_name;
        if (name.empty() || name[0] == '.') continue;
        std::string full = dir + "/" + name;
        if (isDir(full)) { subdirs.push_back(name); continue; }
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
    // A game in a folder of its own ("Dolphin Blue/disc.gdi", how Dreamcast sets
    // usually come) is listed under the folder's name: its playlist or disc
    // sheet, else its only game file. A folder of many ROMs (Hacks/) is not one
    // game and is skipped.
    for (const std::string& sub : subdirs) {
        DIR* sd = ::opendir((dir + "/" + sub).c_str());
        if (!sd) continue;
        std::vector<std::string> files, sheets, lists;
        while (struct dirent* e = ::readdir(sd)) {
            std::string name = e->d_name;
            if (name.empty() || name[0] == '.' || !isFile(dir + "/" + sub + "/" + name)) continue;
            std::string ext = extOf(name);
            if (std::find(sys.extensions.begin(), sys.extensions.end(), ext) == sys.extensions.end()) continue;
            if (ext == "m3u") lists.push_back(name);
            else if (ext == "gdi" || ext == "cue") sheets.push_back(name);
            else files.push_back(name);
        }
        ::closedir(sd);
        std::string pick = lists.size() == 1 ? lists[0] : lists.empty() && sheets.size() == 1 ? sheets[0]
                         : lists.empty() && sheets.empty() && files.size() == 1 ? files[0] : "";
        if (pick.empty()) continue;
        Game g;
        g.file = sub + "/" + pick;
        g.path = dir + "/" + g.file;
        splitTitle(sub, g.title, g.tags);
        games.push_back(g);
    }
    // A .cue's / .gdi's track files and an .m3u's discs are parts of one game:
    // list only the .cue / .gdi / .m3u.
    std::set<std::string> parts;
    for (const Game& g : games) {
        std::string ext = extOf(g.file);
        if (ext != "cue" && ext != "m3u" && ext != "gdi") continue;
        std::ifstream in(g.path);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string ref;
            if (ext == "cue") {
                size_t a = line.find('"'), b = line.rfind('"');
                if (lower(line).find("file") == std::string::npos || a == std::string::npos || b <= a) continue;
                ref = line.substr(a + 1, b - a - 1);
            } else if (ext == "gdi") {
                // "<track> <lba> <type> <sector size> <file> <offset>"; the file may be quoted.
                size_t q = line.find('"');
                if (q != std::string::npos) {
                    size_t e = line.find('"', q + 1);
                    if (e == std::string::npos) continue;
                    ref = line.substr(q + 1, e - q - 1);
                } else {
                    std::istringstream fields(line);
                    std::string f[5];
                    if (!(fields >> f[0] >> f[1] >> f[2] >> f[3] >> f[4])) continue;
                    ref = f[4];
                }
            } else {
                ref = trim(line);
                if (ref.empty() || ref[0] == '#') continue;
            }
            parts.insert(lower(ref.substr(ref.find_last_of('/') + 1)));
            parts.insert(lower(ref));  // "Game (Disc 1)/Game (Disc 1).cue": a disc in its own folder
        }
    }
    if (!parts.empty())
        games.erase(std::remove_if(games.begin(), games.end(),
                                   [&](const Game& g) { return parts.count(lower(g.file)) > 0; }),
                    games.end());
    std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) {
        return lower(a.title + " " + a.tags) < lower(b.title + " " + b.tags);
    });
    return games;
}

// BIOS files a system's cores look for in system/ ("" = none needed).
// ------------------------------------------------------------ BIOS check

namespace {

// MD5 (RFC 1321), for telling a known good BIOS dump from another file.
std::string md5File(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return "";
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20,
                              5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    auto block = [&](const uint8_t* b) {
        uint32_t w[16];
        for (int i = 0; i < 16; ++i) w[i] = b[i * 4] | (b[i * 4 + 1] << 8) | (b[i * 4 + 2] << 16) | ((uint32_t)b[i * 4 + 3] << 24);
        uint32_t a = h[0], bb = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            uint32_t fn;
            int g;
            if (i < 16) { fn = (bb & c) | (~bb & d); g = i; }
            else if (i < 32) { fn = (d & bb) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { fn = bb ^ c ^ d; g = (3 * i + 5) % 16; }
            else { fn = c ^ (bb | ~d); g = (7 * i) % 16; }
            uint32_t t = d;
            d = c;
            c = bb;
            uint32_t x = a + fn + K[i] + w[g];
            bb = bb + ((x << R[i]) | (x >> (32 - R[i])));
            a = t;
        }
        h[0] += a; h[1] += bb; h[2] += c; h[3] += d;
    };
    std::vector<uint8_t> buf(1 << 16);
    uint64_t total = 0;
    size_t n, have = 0;
    uint8_t tail[128];
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
        total += n;
        size_t i = 0;
        if (have) {  // finish the partial block left over from the last read
            while (have < 64 && i < n) tail[have++] = buf[i++];
            if (have == 64) { block(tail); have = 0; }
        }
        for (; i + 64 <= n; i += 64) block(&buf[i]);
        while (i < n) tail[have++] = buf[i++];
    }
    std::fclose(f);
    tail[have++] = 0x80;
    if (have > 56) { while (have < 64) tail[have++] = 0; block(tail); have = 0; }
    while (have < 56) tail[have++] = 0;
    uint64_t bits = total * 8;
    for (int i = 0; i < 8; ++i) tail[56 + i] = (uint8_t)(bits >> (8 * i));
    block(tail);
    char out[33];
    for (int i = 0; i < 16; ++i) std::snprintf(out + i * 2, 3, "%02x", (h[i / 4] >> (8 * (i % 4))) & 0xff);
    return out;
}

struct BiosSpec {
    const char* system;
    std::vector<const char*> names;  // accepted file names, the usual one first
    std::vector<const char*> dirs;   // where the core looks, the suggested one first
    std::vector<const char*> md5s;   // known good dumps; none: any file will do (arcade BIOS zips)
    bool required;
    const char* why;
};

const std::vector<BiosSpec>& biosSpecs() {
    static const std::vector<BiosSpec> list = {
        {"naomi", {"naomi.zip"}, {"roms/naomi/", "system/dc/"}, {}, true, "every NAOMI game needs it"},
        {"atomiswave", {"awbios.zip"}, {"roms/atomiswave/", "system/dc/"}, {}, true, "every Atomiswave game needs it"},
        {"neogeo", {"neogeo.zip"}, {"roms/neogeo/", "system/fbneo/", "system/"}, {}, true,
         "every Neo Geo game needs it (one in roms/arcade/ is copied over for you)"},
        {"psx", {"scph5501.bin", "scph5500.bin", "scph5502.bin", "scph1001.bin"}, {"system/"},
         {"490f666e1afb15b7362b406ed1cea246", "8dd7d5296a650fac7319bce665a6a53c", "32736f17079d0b2b7024407c39bd3050",
          "924e392ed05558ffdb115408c263dccf"},
         false, "runs more games than the built-in BIOS (USA: scph5501, Japan: scph5500, Europe: scph5502)"},
        {"saturn", {"saturn_bios.bin"}, {"system/"}, {"af5828fdff51384f99b3c4926be27762"}, false,
         "runs more games than the built-in BIOS"},
        {"dreamcast", {"dc_boot.bin", "boot.bin"}, {"system/dc/"}, {"e10c53c2f8b90bab96ead2d368858623"}, false,
         "needed for homebrew and some conversions; official discs run without it"},
        {"dreamcast", {"dc_flash.bin", "flash.bin"}, {"system/dc/"}, {"0a93f7940c455905bea6e392dfde92a4"}, false,
         "goes with dc_boot.bin (console settings)"},
        {"gba", {"gba_bios.bin"}, {"system/"}, {"a860e8c0b6d573d191e4ec7db1b1e4f6"}, false,
         "fixes a few games; gpSP has a built-in BIOS"},
        {"gb", {"gb_bios.bin"}, {"system/"}, {"32fbbd84168d3482956eb3c5051637f5"}, false, "boot logo only"},
        {"gbc", {"gbc_bios.bin", "cgb_bios.bin"}, {"system/"}, {"dbfce9db9deaa2567f6a84fde55f9680"}, false, "boot logo only"},
        {"lynx", {"lynxboot.img"}, {"system/"}, {"fcd403db69f54290b51035d82f835e7b"}, false,
         "Handy starts most games without it"},
        {"colecovision", {"colecovision.rom", "coleco.rom", "os7.u2"}, {"system/"}, {"2c66f5911e5b42b8ebe113403548eee7"},
         false, "switches to the Gearcoleco emulator; the cabinet's own one has the BIOS built in"},
    };
    return list;
}

} // namespace

std::vector<BiosCheck> checkBios(const std::string& appDir, const std::vector<std::string>& systems) {
    std::vector<BiosCheck> out;
    for (const BiosSpec& b : biosSpecs()) {
        if (std::find(systems.begin(), systems.end(), b.system) == systems.end()) continue;
        BiosCheck c;
        c.system = b.system;
        c.file = b.names[0];
        c.required = b.required;
        c.why = b.why;
        c.where = b.dirs[0];
        for (const char* d : b.dirs)
            for (const char* n : b.names)
                if (c.foundAt.empty() && isFile(appDir + "/" + d + n)) c.foundAt = std::string(d) + n;
        if (c.foundAt.empty()) c.state = BiosCheck::Missing;
        else if (b.md5s.empty()) c.state = BiosCheck::Ok;
        else {
            const std::string sum = md5File(appDir + "/" + c.foundAt);
            c.state = BiosCheck::Unrecognized;
            for (const char* m : b.md5s)
                if (sum == m) c.state = BiosCheck::Ok;
        }
        out.push_back(c);
    }
    return out;
}

static const char* biosNote(const std::string& id) {
    if (id == "colecovision")
        return "Optional: colecovision.rom (8 KB; coleco.rom and os7.u2 also accepted) for Gearcoleco. "
               "Without it the cabinet's own ColecoVision emulator (built-in BIOS) is used.";
    if (id == "gb") return "Optional: gb_bios.bin (boot logo only).";
    if (id == "gbc") return "Optional: gbc_bios.bin (boot logo only).";
    if (id == "gba")
        return "Optional: gba_bios.bin. gpSP has a built-in BIOS; the original improves compatibility with a few games.";
    if (id == "lynx") return "Recommended: lynxboot.img (512 bytes). Handy can start most games without it.";
    if (id == "pce") return "None for HuCard games (CD games are not supported).";
    if (id == "n64") return "";
    if (id == "saturn")
        return "Optional: saturn_bios.bin. YabaSanshiro has a built-in BIOS replacement; a real one runs more games.";
    if (id == "psp") return "None. PPSSPP's own files (fonts, shaders) go in system/PPSSPP/ - they come with Retro Launcher.";
    if (id == "dreamcast")
        return "dc/dc_boot.bin and dc/dc_flash.bin (in a dc folder inside system/). Official discs run on "
               "Flycast's built-in BIOS replacement; homebrew and conversions (e.g. the Atomiswave ports) need "
               "the real BIOS.";
    if (id == "psx")
        return "Optional: scph5501.bin (USA), scph5500.bin (Japan), scph5502.bin (Europe). PCSX ReARMed has a "
               "built-in BIOS; a real one runs more games.";
    if (id == "naomi")
        return "Required: naomi.zip (some games want their own, e.g. hod2bios.zip), with the games or in system/dc/.";
    if (id == "atomiswave") return "Required: awbios.zip, with the games or in system/dc/.";
    if (id == "neogeo")
        return "Required: neogeo.zip, with the games or in system/fbneo/ (one in roms/arcade/ is copied there for you).";
    if (id == "arcade") return "BIOS zips (neogeo.zip, pgm.zip...) go in roms/arcade/ with the games, not in system/.";
    return "";
}

static void writeIfMissing(const std::string& path, const std::string& text) {
    if (isFile(path)) return;  // never overwrite: the user may have edited it
    std::ofstream out(path);
    out << text;
}

// A README.txt in each folder the user fills, written once.
static void writeGuides(const std::string& appDir) {
    std::string list;
    for (const System& s : systems()) {
        std::string exts;
        for (const std::string& e : s.extensions) if (e != "zip") exts += "." + e + " ";
        list += "  " + s.id + std::string(14 - std::min<size_t>(13, s.id.size()), ' ') + s.name + "  (" + exts + ".zip)\n";
    }
    writeIfMissing(appDir + "/roms/README.txt",
        "Put your games in the folder for their system:\n\n" + list +
        "\nPlain files or .zip (one game per zip). The menu shows the file name\n"
        "without tags like (USA) or [!], and box art is matched by the same file\n"
        "name (see media/README.txt). Retro Launcher ships no games.\n");
    for (const System& s : systems()) {
        std::string exts;
        for (const std::string& e : s.extensions) if (e != "zip") exts += "." + e + ", ";
        std::string bios = biosNote(s.id);
        writeIfMissing(appDir + "/roms/" + s.id + "/README.txt",
            s.name + " games go here.\n\nFile types: " +
            (s.id == "psp" ? exts.substr(0, exts.size() - 2) +
                                 "\n.cso (compressed .iso) or .chd saves space. Don't zip them.\n"
             : s.id == "psx" || s.id == "dreamcast" || s.id == "saturn"
                 ? exts.substr(0, exts.size() - 2) +
                       "\nUse .chd if you can (one small file per disc). A .cue or .gdi needs its track\n"
                       "files beside it. Don't zip disc images.\n" +
                       std::string(s.id == "saturn" ? "Multi-disc games: each disc is its own game (no disc swapping).\n"
                                                    : "Multi-disc games named \"(Disc 1)\", \"(Disc 2)\"... become one game.\n")
                           : exts + ".zip\n") +
            (bios.empty() ? "" : (isArcadeSystem(s.id) ? "BIOS: " : "BIOS (in system/): ") + bios + "\n") +
            (s.id == "arcade"
                 ? std::string("Leave the zips as they are (don't unpack them). Each one is checked against the\n"
                               "ROM lists in cores/ and played with the emulator it is complete for: FBNeo\n"
                               "(current sets), else MAME 2003-Plus (MAME 0.78-era sets). Parent zips\n"
                               "(sf2.zip for sf2ce.zip) go here too. Vertical games play on the playfield.\n")
             : s.id == "neogeo"
                 ? std::string("Leave the zips as they are (FBNeo sets, e.g. mslug.zip, kof98.zip). Each is checked\n"
                               "against FBNeo's ROM list; clones need their parent zip here too. Other arcade\n"
                               "games go in roms/arcade/.\n")
             : s.id == "naomi" || s.id == "atomiswave"
                 ? std::string("Leave the zips as they are (MAME-style sets). Each is checked against Flycast's\n"
                               "own game list. GD-ROM games also need their .chd in a folder named after the\n"
                               "zip, e.g. ikaruga/gdl-0010.chd. Vertical games play on the playfield.\n")
                 : "Core: " + s.cores[0] + (s.cores.size() > 1 ? " (else " + s.cores[1] + ")" : "") + "\n"));
    }
    std::string bios;
    for (const System& s : systems()) {
        std::string note = biosNote(s.id);
        if (!note.empty()) bios += "  " + s.name + ": " + note + "\n";
    }
    writeIfMissing(appDir + "/system/README.txt",
        "BIOS files the emulator cores look for. Copy your own dumps here with\n"
        "exactly these names (Retro Launcher ships none):\n\n" + bios +
        "\nEvery other system needs no BIOS.\n");
    writeIfMissing(appDir + "/cores/README.txt",
        "Optional emulator cores (.so files built for the cabinet). A core here is\n"
        "used instead of the firmware's own; systems the firmware has no core for\n"
        "(Game Boy, GBA, PC Engine, Lynx) need theirs here.\n\n"
        "Cores the launcher looks for, by system:\n\n" +
        [] {
            std::string t;
            for (const System& s : systems()) {
                t += "  " + s.name + ": ";
                for (size_t i = 0; i < s.cores.size(); ++i) t += (i ? ", " : "") + s.cores[i];
                t += "\n";
            }
            return t;
        }() +
        "\nPrebuilt cores from the libretro buildbot do NOT load on the cabinet\n"
        "(too new a glibc). Use the cores from a Retro Launcher release or build\n"
        "them with tools/build-cores.sh: https://github.com/Bla1ze/retro-launcher\n");
    writeIfMissing(appDir + "/media/README.txt",
        "Artwork, per system folder (same names as in roms/):\n\n"
        "  media/<system>/boxart/<ROM name>.png or .jpg   cover for one game\n"
        "  media/<system>/bezel.png                       bezel for the whole system\n"
        "  media/<system>/bezels/<ROM name>.png           bezel for one game\n"
        "  media/<system>/console.png or .jpg             console photo for the DMD\n\n"
        "<ROM name> is the game's file name without its extension, e.g. roms/nes/\n"
        "Tetris (USA).zip -> media/nes/boxart/Tetris (USA).png. libretro-thumbnails\n"
        "names (& * / : ` < > ? \\ | \" replaced by _) also work.\n\n"
        "Bezels use The Bezel Project format: a PNG (usually 1920x1080) with a transparent\n"
        "window where the game goes.\n\n"
        "Retro Launcher ships no artwork. The scripts in tools/ download it for\n"
        "you: https://github.com/Bla1ze/retro-launcher#artwork\n");
}

void ensureFolders(const std::string& appDir) {
    for (const char* sub : {"/roms", "/saves", "/system", "/cores", "/data", "/media"})
        ::mkdir((appDir + sub).c_str(), 0755);
    for (const System& s : systems()) {
        ::mkdir((appDir + "/roms/" + s.id).c_str(), 0755);
        ::mkdir((appDir + "/saves/" + s.id).c_str(), 0755);
        ::mkdir((appDir + "/media/" + s.id).c_str(), 0755);
    }
    writeGuides(appDir);
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
                    "# bars = ambient (glow + bokeh from the game's colors) or black.\n");
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
void Settings::erase(const std::string& key) { m_values.erase(key); }

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
    log("running %s", selfPath().c_str());  // also fixes the path before an update can replace the file
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

std::string findCoreFile(const std::string& appDir, const std::string& coreFile, std::string& where) {
    System one;
    one.cores = {coreFile};
    return findCore(appDir, one, where);
}

bool hasColecoBios(const std::string& appDir) {
    for (const char* d : {"/system/", "/system/gearcoleco/"})
        for (const char* n : {"colecovision.rom", "coleco.rom", "os7.u2"})
            if (isFile(appDir + d + n)) return true;
    return false;
}

std::string findCore(const std::string& appDir, const System& sys, std::string& where) {
    static const char* kDirs[] = {"/upgrade/opt/retroplayer/core", "/upgrade/retroplayer/core",
                                  "/app/retroplayer/core", "/emulator"};
    // ColecoVision: Gearcoleco needs the OS-7 BIOS in system/; without it the
    // firmware's own core (libcv, which has the BIOS built in) runs the game.
    if (sys.id == "colecovision") {
        if (!hasColecoBios(appDir))
            for (const char* dir : kDirs) {
                std::string p = std::string(dir) + "/libcv.so";
                if (::access(p.c_str(), R_OK) == 0) {
                    where = std::string("firmware ") + dir + " (no ColecoVision BIOS in system/ for Gearcoleco)";
                    return p;
                }
            }
    }
    // 1. Our own cores (copied off the no-exec stick before loading).
    for (const std::string& c : sys.cores) {
        std::string src = appDir + "/cores/" + c;
        if (!isFile(src)) continue;
        ::mkdir("/tmp/retrofe/cores", 0755);
        std::string dst = "/tmp/retrofe/cores/" + c;
        // Already copied this session (it lives until the app exits): FBNeo is
        // 80 MB, seconds to copy off the stick on every start.
        struct stat ss, ds;
        if (::stat(src.c_str(), &ss) == 0 && ::stat(dst.c_str(), &ds) == 0 && ss.st_size == ds.st_size &&
            ds.st_mtime >= ss.st_mtime) { where = "app cores/ (already copied)"; return dst; }
        if (copyFile(src, dst)) { where = "app cores/"; return dst; }
        log("could not copy %s to %s: %s", src.c_str(), dst.c_str(), std::strerror(errno));
    }
    // 2. The firmware's core folders, in the order tableDB_retroplayer.sh uses.
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

void execUpdated(const std::string& appDir, const std::string& message) {
    // The new version's file, wherever this process was started from.
    const std::string program = appDir + "/retro-launcher.elf";
    log("-> menu, restarting %s (%s)", program.c_str(), message.c_str());
    exec({"retro-launcher", "--menu", appDir, "", "0", message}, program);
}

void execPlay(const std::string& appDir, const std::string& sys, const std::string& romPath, ScreenId screen,
              int index, const std::string& returnTo, const std::string& core) {
    log("-> play %s on %s%s%s", romPath.c_str(), screenName(screen), core.empty() ? "" : " with ", core.c_str());
    exec({"retro-launcher", "--play", appDir, sys, romPath, screenName(screen), std::to_string(index),
          returnTo.empty() ? sys : returnTo, core});
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
