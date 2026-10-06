#pragma once

#include "Library.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

// The Arcade Control Panel's trackball is a USB mouse
// (/dev/input/by-id/usb-0838_8918-event-mouse, as the firmware's own player
// reads it); any other mouse works too. Read straight from evdev once a frame
// and given to cores that ask for a mouse: MAME 2003-Plus (its xy_device set to
// "mouse") and FBNeo for trackball / spinner / paddle games (port set to its
// "mouse, ball only" device). The menu scrolls its lists with it too.
struct Trackball {
    int fd = -1;
    float scale = 1.0f, accX = 0.0f, accY = 0.0f;
    int16_t dx = 0, dy = 0;
    bool left = false, right = false, middle = false;

    static bool isMouse(int f) {
        unsigned long rel = 0, key[(KEY_MAX + 1) / (8 * sizeof(unsigned long)) + 1] = {};
        if (::ioctl(f, EVIOCGBIT(EV_REL, sizeof(rel)), &rel) < 0) return false;
        ::ioctl(f, EVIOCGBIT(EV_KEY, sizeof(key)), key);
        const size_t bits = 8 * sizeof(unsigned long);
        bool hasLeft = (key[BTN_LEFT / bits] >> (BTN_LEFT % bits)) & 1;
        return (rel & (1ul << REL_X)) && (rel & (1ul << REL_Y)) && hasLeft;
    }
    bool open() {
        std::vector<std::string> tries = {"/dev/input/by-id/usb-0838_8918-event-mouse"};
        if (DIR* d = ::opendir("/dev/input/by-id")) {  // other USB mice
            while (struct dirent* e = ::readdir(d)) {
                std::string n = e->d_name;
                if (n.size() > 12 && n.compare(n.size() - 12, 12, "-event-mouse") == 0) tries.push_back("/dev/input/by-id/" + n);
            }
            ::closedir(d);
        }
        for (int i = 0; i < 32; ++i) tries.push_back("/dev/input/event" + std::to_string(i));
        for (const std::string& p : tries) {
            int f = ::open(p.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (f < 0) continue;
            if (!isMouse(f)) { ::close(f); continue; }
            char name[128] = "?";
            ::ioctl(f, EVIOCGNAME(sizeof(name)), name);
            Library::log("trackball: %s (%s)", p.c_str(), name);
            fd = f;
            return true;
        }
        Library::log("trackball: none found");
        return false;
    }
    // Once per frame: this frame's movement (scaled; fractions carried over).
    // `use` false (a menu is open) drops the movement.
    void frame(bool use) {
        dx = dy = 0;
        if (fd < 0) return;
        struct input_event ev[64];
        ssize_t n;
        while ((n = ::read(fd, ev, sizeof(ev))) > 0) {
            for (ssize_t i = 0; i < n / (ssize_t)sizeof(ev[0]); ++i) {
                if (ev[i].type == EV_REL && ev[i].code == REL_X) accX += ev[i].value * scale;
                else if (ev[i].type == EV_REL && ev[i].code == REL_Y) accY += ev[i].value * scale;
                else if (ev[i].type == EV_KEY && ev[i].code == BTN_LEFT) left = ev[i].value != 0;
                else if (ev[i].type == EV_KEY && ev[i].code == BTN_RIGHT) right = ev[i].value != 0;
                else if (ev[i].type == EV_KEY && ev[i].code == BTN_MIDDLE) middle = ev[i].value != 0;
            }
        }
        if (!use) { accX = accY = 0.0f; return; }
        auto take = [](float& acc) {
            float whole = std::max(-32767.0f, std::min(32767.0f, std::trunc(acc)));
            acc -= whole;
            return (int16_t)whole;
        };
        dx = take(accX);
        dy = take(accY);
    }
};
