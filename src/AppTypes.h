#pragma once

#include <string>

// A snapshot of what's playing, handed to the secondary-panel output (backglass
// / DMD) each frame. Kept minimal and SDL-free so both the platform-independent
// suite and the raw-framebuffer PanelOutput can share it.
struct PanelNowPlaying {
    enum class Kind { Idle, Local, Radio };
    Kind kind = Kind::Idle;
    std::string title;   // track title (Local) or station name (Radio)
    std::string artist;  // track artist (Local); empty otherwise
    std::string artPath; // local track path for cover extraction; empty otherwise
};

// Keep layout in floating point while rendering through older SDL2 integer
// rectangles. This avoids newer SDL2 APIs that may not exist on the firmware.
struct FRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

// The Jukebox has a title menu, two source browsers, a now-playing view, and
// the shared exit confirmation. Keeping screens explicit mirrors the other SDK
// samples (see arcade-test-app's AppScreen).
enum class AppScreen {
    MainMenu,
    Radio,
    Search,
    Favorites,
    LocalFiles,
    NowPlaying,
    QuitConfirm
};

// App translates physical buttons into these results. The platform layer only
// needs to know whether the suite asked to quit back to the firmware.
enum class MenuCommand {
    None,
    QuitGame
};
