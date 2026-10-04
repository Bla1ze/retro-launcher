#pragma once

#include "DrmKms.h"

#include <SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

// Which physical screen is which, per cabinet model.
//
// Ported from the AtGames External Applications SDK 0.3.1 `sdk/displays`
// (https://www.atgames.net/features/external-apps; DisplayTopology +
// SdlDisplay), minus its libdrm dependency — this app already talks to DRM
// through raw ioctls, and its toolchain has no libdrm.
//
// Two things from there matter:
//
//  1. The roles of the first three DRM connectors are MODEL-SPECIFIC, keyed on
//     the launcher's ATGAMES_DEVICE_TYPE_ID:
//
//       profile   slot 0       slot 1       slot 2
//       HA9920    Main         DMD (90°)    Backglass
//       HA9919    Backglass    DMD (90°)    Main        <- the swap we chased
//
//     Nothing about a screen's resolution reveals its mounting or its role, so
//     guessing from shape or size is exactly what this replaces.
//
//  2. `ForceConnectID` tells the CABINET'S SDL which connector to open, before
//     SDL_Init. Upstream SDL ignores it (it just takes the first connected
//     connector, which is the backglass on an HA9919) — the firmware's vendor
//     SDL honors it, which is what puts our window on the playfield.
namespace DisplayProfile {

struct Screen {
    bool available = false;
    uint32_t connectorId = 0;
    int width = 0;
    int height = 0;
    int rotationDegrees = 0; // clockwise, applied by the app when presenting
};

struct Topology {
    std::string model;            // ATGAMES_DEVICE_TYPE_ID, "" when unset
    bool known = false;           // model matched a profile
    bool keepFirmwareDisplay = false; // HD ALPs: draw on a free overlay layer
    Screen main;
    Screen backglass;
    Screen dmd;
};

namespace detail {

struct Slot {
    bool present = false;
    uint32_t connectorId = 0;
    int width = 0;
    int height = 0;
};

// The first three connectors in kernel enumeration order, connected and with a
// mode — the same slots the SDK's profiles are written against.
inline void readSlots(Slot (&slots)[3])
{
    const int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    drm_mode_card_res res;
    std::memset(&res, 0, sizeof(res));
    std::vector<uint32_t> conns;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0 && res.count_connectors > 0) {
        conns.resize(res.count_connectors);
        res.connector_id_ptr = reinterpret_cast<uint64_t>(conns.data());
        res.count_crtcs = res.count_encoders = res.count_fbs = 0;
        res.crtc_id_ptr = res.encoder_id_ptr = res.fb_id_ptr = 0;
        if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
            conns.clear();
        }
    }
    const int n = static_cast<int>(conns.size()) < 3 ? static_cast<int>(conns.size()) : 3;
    for (int i = 0; i < n; ++i) {
        drm_mode_get_connector cn;
        std::memset(&cn, 0, sizeof(cn));
        cn.connector_id = conns[static_cast<std::size_t>(i)];
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &cn) != 0) continue;
        if (cn.connection != 1 || cn.count_modes == 0) continue;
        std::vector<drm_mode_modeinfo> modes(cn.count_modes);
        cn.modes_ptr = reinterpret_cast<uint64_t>(modes.data());
        cn.count_encoders = cn.count_props = 0;
        cn.encoders_ptr = cn.props_ptr = cn.prop_values_ptr = 0;
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &cn) != 0 || cn.count_modes == 0) continue;
        slots[i].present = true;
        slots[i].connectorId = cn.connector_id;
        slots[i].width = modes[0].hdisplay;
        slots[i].height = modes[0].vdisplay;
    }
    close(fd);
}

inline Screen toScreen(const Slot& s, int rotation = 0)
{
    Screen out;
    if (!s.present) {
        return out;
    }
    out.available = true;
    out.connectorId = s.connectorId;
    out.width = s.width;
    out.height = s.height;
    out.rotationDegrees = rotation;
    return out;
}

inline bool isHdPinball(const std::string& m)
{
    return m == "HA8818" || m == "HA8819" || m == "HA8819C" || m == "HA8820";
}

} // namespace detail

// Pinball cabinets mount Main portrait; content is authored portrait and
// rotated 90° clockwise when presented (what this app has always done).
constexpr int kPinballMainRotation = 90;
constexpr int kDmdRotation = 90;

inline Topology detect()
{
    Topology t;
    const char* env = SDL_getenv("ATGAMES_DEVICE_TYPE_ID");
    t.model = env ? env : "";

    detail::Slot slots[3];
    detail::readSlots(slots);

    if (t.model == "HA9920") {
        // Pinball: Main, DMD, Backglass.
        t.main = detail::toScreen(slots[0], kPinballMainRotation);
        t.dmd = detail::toScreen(slots[1], kDmdRotation);
        t.backglass = detail::toScreen(slots[2]);
        t.known = true;
    } else if (t.model == "HA9919") {
        // Pinball with a different slot order: Backglass, DMD, Main.
        t.backglass = detail::toScreen(slots[0]);
        t.dmd = detail::toScreen(slots[1], kDmdRotation);
        t.main = detail::toScreen(slots[2], kPinballMainRotation);
        t.known = true;
    } else if (t.model == "HAB4000P") {
        // Pinball: Main (4K connector preferred), Backglass.
        t.main = detail::toScreen(slots[0].present ? slots[0] : slots[2], kPinballMainRotation);
        t.backglass = detail::toScreen(slots[1]);
        t.known = true;
    } else if (detail::isHdPinball(t.model)) {
        // HD ALPs use the first two CONNECTED outputs, whichever slots those are.
        int mainSlot = -1, bgSlot = -1;
        for (int i = 0; i < 3; ++i) {
            if (!slots[i].present) continue;
            if (mainSlot < 0) mainSlot = i;
            else { bgSlot = i; break; }
        }
        if (mainSlot >= 0) t.main = detail::toScreen(slots[mainSlot], kPinballMainRotation);
        if (bgSlot >= 0) {
            const bool portrait = slots[bgSlot].height > slots[bgSlot].width;
            t.backglass = detail::toScreen(slots[bgSlot], portrait ? 90 : 0);
            // Full-size HD keeps the firmware's screen setup and draws on a
            // free overlay layer; Micro (portrait backglass) does not.
            t.keepFirmwareDisplay = (t.model != "HA8818") && !portrait;
        }
        t.known = mainSlot >= 0;
    } else if (t.model == "HAB4000U" || t.model == "HA9910" || t.model == "HA9900") {
        // Arcade: Main only as far as this app is concerned.
        t.main = detail::toScreen(slots[0].present ? slots[0] : slots[2], kPinballMainRotation);
        t.known = t.main.available;
    }
    return t;
}

// Must run BEFORE SDL_Init(SDL_INIT_VIDEO). Tells the cabinet's SDL which
// connector to open; harmless where that SDL build ignores it.
inline void prepareSdlMain(const Topology& t)
{
    if (!t.main.available || t.main.connectorId == 0) {
        return;
    }
    const std::string id = std::to_string(t.main.connectorId);
    SDL_setenv("ForceConnectID", id.c_str(), 1);
    if (t.keepFirmwareDisplay) {
        SDL_setenv("SDL2_DISPLAY_PLANE_TYPE", "OVERLAY", 1);
    }
}

} // namespace DisplayProfile
