#pragma once

#include "DisplayProfile.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The screens around whatever owns SDL's window:
//   Browse mode (menu on the playfield):   backglass + DMD show the highlighted game.
//   Playing mode (game on the backglass):  playfield shows a "Now playing" card with
//                                          the cover and the game's controls, and the
//                                          DMD shows the title. Driven directly over KMS with SDL's own card0 fd,
// the technique Jukebox ships (see its PanelOutput):
//   backglass: box art (or a snap/title screen) over a blurred, darkened copy
//              of itself, with the game's title; a styled title card when the
//              game has no art;
//   DMD:       the system name over the game's title, Neon gradient.
// Recomposed only when the highlighted item has been stable briefly, and on a
// worker thread (latest item wins), so the menu never waits on image decoding
// or the per-pixel panel work.
class GamePanels {
public:
    struct Item {
        std::string key;      // identity; a change triggers a recompose
        std::string system;   // "Super Nintendo"
        std::string title;    // "Aladdin" (empty on the systems list)
        std::string detail;   // "(USA)" or "48 games"
        std::string artPath;  // image file, or empty
        std::vector<std::pair<std::string, std::string>> controls;  // Playing: "A" -> "B button"
        std::string consolePath;  // Playing: photo of the console for the DMD, or empty
    };
    enum class Mode { Browse, Playing };

    ~GamePanels();
    int init(const DisplayProfile::Topology& topology, Mode mode = Mode::Browse);
    bool active() const { return !m_panels.empty(); }

    // Call every frame with what is highlighted; dt in seconds.
    void show(const Item& item, float dt);

private:
    struct Panel {
        uint32_t connId = 0, crtcId = 0, fbId = 0, handle = 0, pitch = 0;
        int w = 0, h = 0;      // physical buffer
        int vw = 0, vh = 0;    // as seen (landscape)
        bool rot = false;      // portrait-native buffer: present rotated
        enum class Role { Backglass, Dmd, Playfield } role = Role::Backglass;
        uint8_t* map = nullptr;
        std::size_t size = 0;
        std::vector<uint8_t> canvas;  // vw*vh RGBA
    };

    uint32_t pickCrtc(uint32_t currentEncoder, const std::vector<uint32_t>& encoders, uint32_t count,
                      uint32_t mainConnector);
    void compose(const Item& item);
    void composeBackglass(Panel& p, const Item& item, const std::vector<uint8_t>& art, int aw, int ah);
    void composeDmd(Panel& p, const Item& item, const std::vector<uint8_t>& console, int cw, int ch);
    void composePlayfield(Panel& p, const Item& item, const std::vector<uint8_t>& art, int aw, int ah);
    void present(Panel& p);

    void worker();

    Mode m_mode = Mode::Browse;
    int m_fd = -1;  // SDL's card0 fd; never closed here
    std::vector<Panel> m_panels;  // touched only by the worker once it runs
    std::string m_shownKey = "\x01", m_pendingKey;
    float m_stable = 0.0f;

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    Item m_job;
    bool m_hasJob = false, m_stop = false;
};

// Finds art for a ROM under <appDir>/media/<system>/: boxart, then snaps, then
// title screens, in our folder names or libretro-thumbnails' (Named_Boxarts...),
// as .png or .jpg, by the ROM's file name or its libretro-sanitized form.
std::string findArt(const std::string& appDir, const std::string& system, const std::string& romFile);

// media/<system>/console.png or .jpg, or empty.
std::string findConsoleArt(const std::string& appDir, const std::string& system);
