#pragma once

// Everything the launcher knows that isn't drawing: the console table, the
// app-folder layout, ROM scanning, the settings file, logging, core lookup and
// the hand-off between the menu process and the game process.
//
// Layout, all inside the app's own folder (external/retro-launcher/):
//   roms/<system>/     ROMs (plain or .zip)
//   saves/<system>/    battery saves
//   system/            BIOS files the cores ask for
//   cores/             optional libretro cores that override the firmware's
//   data/              settings.cfg and launcher.log

#include <map>
#include <string>
#include <vector>

namespace Library {

enum class ScreenId { Playfield, Backglass, Dmd };

const char* screenName(ScreenId s);          // "playfield" / "backglass" / "dmd"
const char* screenLabel(ScreenId s);         // "PLAYFIELD" / "BACKGLASS" / "DMD"
bool parseScreen(const std::string& s, ScreenId& out);

struct System {
    std::string id;        // also the roms/ and saves/ folder name
    std::string name;      // shown in the menu
    std::string shortName; // monogram letter(s)
    std::vector<std::string> cores;       // libretro core file names, preferred first
    std::vector<std::string> extensions;  // lowercase, no dot ("zip" is always accepted)
    float aspect;          // display aspect for the picture
    bool verified;         // played on a cabinet with this launcher
};

const std::vector<System>& systems();

// What each cabinet button does in a system's games, for the playfield card.
// Mirrors the player's mapping (cabinet A/B/X/Y -> libretro B/A/Y/X).
std::vector<std::pair<std::string, std::string>> controlHints(const std::string& systemId);
const System* findSystem(const std::string& id);

struct Game {
    std::string file;   // file name inside roms/<system>/
    std::string path;   // full path
    std::string title;  // file name without extension or (tags)
    std::string tags;   // "(USA) [!]" etc., shown smaller
    // Arcade (see Arcade.h): the emulator detected for this set, every one that
    // can run it, its orientation, and what is wrong with it ("" if nothing).
    bool arcade = false, vertical = false;
    std::string core, problem;
    std::string altTitle;  // the other emulator's name for it, tried for box art too
    std::vector<std::string> cores;
};

std::vector<Game> scanGames(const std::string& appDir, const System& sys);

// Creates roms/<system>/, saves/<system>/, system/, cores/, data/. A read-only
// stick is tolerated (folders that already exist are all that's needed).
void ensureFolders(const std::string& appDir);

// Settings: per-system default screen and per-game override, plus optional
// per-screen picture rotation. Stored as key = value in data/settings.cfg.
class Settings {
public:
    void load(const std::string& appDir);
    bool save() const;

    ScreenId systemScreen(const std::string& sys) const;
    void setSystemScreen(const std::string& sys, ScreenId s);

    bool gameScreen(const std::string& sys, const std::string& file, ScreenId& out) const;
    void setGameScreen(const std::string& sys, const std::string& file, ScreenId s);
    void clearGameScreen(const std::string& sys, const std::string& file);

    ScreenId screenFor(const std::string& sys, const std::string& file) const;
    int rotation(ScreenId s, int fallback) const;
    std::string value(const std::string& key, const std::string& fallback) const;
    void set(const std::string& key, const std::string& value);

private:
    std::string m_path;
    std::map<std::string, std::string> m_values;
};

// Log to data/launcher.log and stdout (stdout reaches logs/output.txt when the
// app XML has <logging>true</logging>).
void openLog(const std::string& appDir, const char* mode);
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int logFd();  // the log file's descriptor (-1 if none), for signal handlers

// Finds a core for the system: the app's cores/ folder first (copied to
// /tmp/retrofe/cores because the stick is mounted no-exec), then the firmware's
// own core folders. Returns "" when none is found; `where` describes the pick.
std::string findCore(const std::string& appDir, const System& sys, std::string& where);
// The same for one named core file (an arcade game's pick).
std::string findCoreFile(const std::string& appDir, const std::string& coreFile, std::string& where);

// The real path of this binary (/tmp/retrofe/application on the cabinet), so
// exec keeps the process name the firmware started instead of "exe".
const std::string& selfPath();

// Replace this process (same PID, which the firmware's launcher waits on).
// Never returns on success.
void execMenu(const std::string& appDir, const std::string& sys, int index, const std::string& message);
// `returnTo` is the menu place to come back to ("" = the game's system list,
// "@recent" = Recently played); `index` is the row there.
// `core` names the core file to use ("" = the system's first available).
void execPlay(const std::string& appDir, const std::string& sys, const std::string& romPath, ScreenId screen,
              int index, const std::string& returnTo = "", const std::string& core = "");

// Favorites: data/favorites.txt, same format, any order.
std::vector<std::pair<std::string, std::string>> loadFavorites(const std::string& appDir);
void saveFavorites(const std::string& appDir, const std::vector<std::pair<std::string, std::string>>& list);

// Recently played: data/recent.txt, "<system>\t<rom file>" per line, newest first.
std::vector<std::pair<std::string, std::string>> loadRecent(const std::string& appDir);
void pushRecent(const std::string& appDir, const std::string& sys, const std::string& file);

} // namespace Library
