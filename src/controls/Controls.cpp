// Based on the controls helper (sdk/controls) from the AtGames External
// Applications SDK, https://www.atgames.net/features/external-apps, modified
// for Retro Launcher (both input devices opened, full key_mapping.ini fallback,
// hat handling). The SDK carries no licence file; this file is included with
// attribution to AtGames.

#include "Controls.h"

#include <cstring>

#include <fstream>
#include <string>

namespace AtGames {
namespace {

// CE writes custom controller mappings here through Controller Settings. The
// file uses SDL's standard gamecontrollerdb format, which means SDL can turn a
// device-specific GUID/button layout into semantic buttons such as A, B, Start,
// Back, and D-pad directions.
const char* kControllerMappingFiles[] = {
    "/userdata/customer_controller_db_3rd.txt",
    "/tmp/customer_controller_db_3rd.txt",
    "customer_controller_db_3rd.txt"
};

// CE also writes a human-readable mapping file. It is useful as a fallback when
// the firmware launches an app in a context where a mapped button still arrives
// as a joystick event instead of a GameController event.
const char* kControllerSettingFiles[] = {
    "/userdata/key_mapping.ini",
    "/tmp/key_mapping.ini",
    "key_mapping.ini"
};

// SDL reports trigger-style flippers as signed axis values on some controller
// mappings. A high threshold keeps resting noise from producing page actions.
constexpr Sint16 kTriggerPressedThreshold = 16000;

// CE's virtual controller. It aggregates the cabinet's own controls, so when a
// machine also exposes a USB arcade control panel this is the one to open.
const char* kPreferredDeviceName = "ATG game console";

// -----------------------------------------------------------------------------
bool triggerPressed(Sint16 value)
{
    return value > kTriggerPressedThreshold;
}

// -----------------------------------------------------------------------------
std::string trim(const std::string& value)
{
    const std::string whitespace = " \t\r\n";
    const std::size_t first = value.find_first_not_of(whitespace);
    if (first == std::string::npos) {
        return "";
    }
    const std::size_t last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

// -----------------------------------------------------------------------------
int parseSdlButtonIndex(const std::string& value)
{
    // key_mapping.ini stores SDL buttons as B<number>. Empty mappings or
    // hat/axis mappings are ignored here because this fallback is only for
    // missing button-style navigation controls.
    const std::string trimmed = trim(value);
    if (trimmed.size() < 2 || trimmed[0] != 'B') {
        return -1;
    }

    for (std::size_t index = 1; index < trimmed.size(); ++index) {
        if (trimmed[index] < '0' || trimmed[index] > '9') {
            return -1;
        }
    }

    return std::stoi(trimmed.substr(1));
}

} // namespace

// -----------------------------------------------------------------------------
Controls::~Controls()
{
    // Centralizing cleanup in close() keeps the destructor and explicit
    // shutdown path identical.
    close();
}

// -----------------------------------------------------------------------------
bool Controls::open()
{
    // Make repeated calls safe and prevent leaking a previously opened handle.
    close();

    // CE may already have created SDL-compatible mappings for the connected
    // cabinet controls. Registering those mappings first gives SDL a chance to
    // expose the device through the semantic GameController API instead of the
    // raw joystick fallback.
    registerControllerMappings();
    loadControllerSettingFallbacks();

    // SDL_NumJoysticks() lists all joystick-like devices. We only open devices
    // that SDL recognizes as GameControllers, because that is the semantic
    // virtual-controller layer shared with CE.
    const int deviceCount = SDL_NumJoysticks();

    // Named controls such as A, Start, Back, and D-pad are portable. Raw button
    // indexes are intentionally ignored because they differ between cabinets.
    // Record what SDL enumerated, for on-screen diagnosis.
    m_deviceReport.clear();
    for (int index = 0; index < deviceCount; ++index) {
        const char* n = SDL_JoystickNameForIndex(index);
        if (!m_deviceReport.empty()) {
            m_deviceReport += ", ";
        }
        m_deviceReport += n ? n : "?";
        if (!SDL_IsGameController(index)) {
            m_deviceReport += " (unmapped)";
        }
    }

    // Open EVERY recognised controller.
    //
    // A cabinet can expose both CE's virtual controller and the USB arcade
    // control panel, and the physical controls are split between them. SDL only
    // emits events for devices it has opened, so opening a single device left
    // the other one's buttons silently dead -- and choosing a "preferred"
    // device merely swapped which half stopped working.
    //
    // Opening both is safe here because the two devices are distinct hardware:
    // a given physical button belongs to exactly one of them, so no press is
    // reported twice. event() maps semantic buttons and never inspects which
    // device a press came from, so it needs no change.
    //
    // CE's virtual device is opened first so it becomes the primary handle for
    // horizontal() and the name shown in diagnostics.
    for (int pass = 0; pass < 2; ++pass) {
        for (int index = 0; index < deviceCount; ++index) {
            if (!SDL_IsGameController(index)) {
                continue;
            }
            const char* candidate = SDL_GameControllerNameForIndex(index);
            const bool preferred =
                candidate && std::strcmp(candidate, kPreferredDeviceName) == 0;
            if ((pass == 0) != preferred) {
                continue;   // pass 0 takes the preferred device, pass 1 the rest
            }

            SDL_GameController* handle = SDL_GameControllerOpen(index);
            if (!handle) {
                continue;   // detected but unopenable; try the next one
            }
            m_controllers.push_back(handle);

            if (!m_controller) {
                m_controller = handle;
                const char* name = SDL_GameControllerName(handle);
                m_deviceName = name ? name : "";
            }
        }
    }

    if (!m_controllers.empty()) {
        // Re-read the ini now the primary device is known, so the fallback
        // table comes from its section rather than a guess.
        loadControllerSettingFallbacks();
        return true;
    }

    // If no semantic mapping exists, the app keeps keyboard input for PC
    // development but does not invent a raw joystick layout on device.
    return hasDevice();
}

// -----------------------------------------------------------------------------
void Controls::close()
{
    // SDL requires the matching close function for each API. Never close a
    // GameController handle with SDL_JoystickClose(), or the reverse.
    for (SDL_GameController* handle : m_controllers) {
        if (handle) {
            SDL_GameControllerClose(handle);
        }
    }
    m_controllers.clear();
    m_controller = nullptr;
    m_leftTriggerHeld = false;
    m_rightTriggerHeld = false;
}

// -----------------------------------------------------------------------------
ControlEvent Controls::event(
    const SDL_Event& input,
    bool allowJoystickAxis,
    bool allowPageButtonsAsDirections) const
{
    // allowJoystickAxis remains in the public API for older SDK callers, but
    // this helper does not map raw joystick axes. Trigger axes below are SDL
    // GameController semantic axes, not raw joystick fallback input.
    (void)allowJoystickAxis;
    (void)allowPageButtonsAsDirections;

    // Keyboard input is primarily useful when developing the game on a PC.
    // Supporting it here also gives every sample the same desktop controls.
    if (input.type == SDL_KEYDOWN) {
        switch (input.key.keysym.sym) {
        case SDLK_UP:
            return ControlEvent::DpadUp;
        case SDLK_DOWN:
            return ControlEvent::DpadDown;
        case SDLK_LEFT:
            return ControlEvent::DpadLeft;
        case SDLK_RIGHT:
            return ControlEvent::DpadRight;
        case SDLK_RETURN:
        case SDLK_SPACE:
            return ControlEvent::A;
        case SDLK_ESCAPE:
        case SDLK_BACKSPACE:
            return ControlEvent::Back;
        default:
            return ControlEvent::None;
        }
    }

    // GameController events use SDL's semantic button names. These mappings do
    // not depend on the physical button number reported by the device.
    if (input.type == SDL_CONTROLLERBUTTONDOWN) {
        switch (input.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            return ControlEvent::DpadUp;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            return ControlEvent::DpadDown;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            return ControlEvent::DpadLeft;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            return ControlEvent::DpadRight;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            return ControlEvent::LeftShoulder;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            return ControlEvent::RightShoulder;
        case SDL_CONTROLLER_BUTTON_A:
            return ControlEvent::A;
        case SDL_CONTROLLER_BUTTON_B:
            return ControlEvent::B;
        case SDL_CONTROLLER_BUTTON_X:
            return ControlEvent::X;
        case SDL_CONTROLLER_BUTTON_Y:
            return ControlEvent::Y;
        case SDL_CONTROLLER_BUTTON_START:
            return ControlEvent::Start;
        case SDL_CONTROLLER_BUTTON_GUIDE:
            return ControlEvent::Guide;
        case SDL_CONTROLLER_BUTTON_BACK:
            return ControlEvent::Back;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:
            // CE exports XCNT_GAMEPAD_LEFT_THUMB / REWIND as SDL leftstick.
            // Keep it as a distinct SDK event so apps can decide whether it
            // means "back", "rewind", or a gameplay action.
            return ControlEvent::Rewind;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
            // CE exports XCNT_GAMEPAD_RIGHT_THUMB / REWIND2 as SDL rightstick.
            // Exposing it separately keeps both thumb-style CE controls
            // available without falling back to raw button indexes.
            return ControlEvent::Rewind2;
        default:
            return ControlEvent::None;
        }
    }

    // Some firmware launch paths still emit selected CE-mapped controls as
    // raw joystick button events. Use the device-generated key_mapping.ini to
    // translate those buttons without hard-coding a cabinet-specific layout.
    //
    // This fallback only applies when no GameController is open. When SDL
    // does expose one, the exact same physical press already produced a
    // CONTROLLERBUTTONDOWN event above; consulting the raw joystick fallback
    // too would map the same press twice (e.g. two Rewind events for one
    // press of the REWIND control, when key_mapping.ini and the GameController
    // mapping both cover it).
    if (input.type == SDL_JOYBUTTONDOWN) {
        if (m_controller) {
            return ControlEvent::None;
        }
        return eventFromMappedJoystickButton(input.jbutton.button);
    }

    // Same guard: when a GameController is open the identical press already
    // arrived as SDL_CONTROLLERBUTTONDOWN, so consulting the hat too would
    // double-fire every direction.
    if (input.type == SDL_JOYHATMOTION) {
        if (m_controller) {
            return ControlEvent::None;
        }
        return eventFromJoystickHat(input.jhat.value);
    }

    // Pinball-style cabinets can expose the second left/right flipper pair as
    // GameController triggers. Treat the press edge like page left/right while
    // ignoring the release edge so one squeeze does not double-fire.
    if (input.type == SDL_CONTROLLERAXISMOTION) {
        if (input.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) {
            const bool pressed = triggerPressed(input.caxis.value);
            if (pressed && !m_leftTriggerHeld) {
                m_leftTriggerHeld = true;
                return ControlEvent::LeftTrigger;
            }
            m_leftTriggerHeld = pressed;
            return ControlEvent::None;
        }
        if (input.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
            const bool pressed = triggerPressed(input.caxis.value);
            if (pressed && !m_rightTriggerHeld) {
                m_rightTriggerHeld = true;
                return ControlEvent::RightTrigger;
            }
            m_rightTriggerHeld = pressed;
            return ControlEvent::None;
        }
    }

    // Most SDL events (window, rendering, audio, button release, and so on)
    // are intentionally unrelated to this helper.
    return ControlEvent::None;
}

// -----------------------------------------------------------------------------
float Controls::horizontal() const
{
    // Poll every opened device: the D-pad may live on one and the flippers on
    // the other.
    for (SDL_GameController* handle : m_controllers) {
        // D-pad is part of the confirmed virtual-controller contract and is
        // safe to read continuously for simple left/right gameplay.
        const bool left =
            SDL_GameControllerGetButton(handle, SDL_CONTROLLER_BUTTON_DPAD_LEFT) ||
            SDL_GameControllerGetButton(handle, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ||
            triggerPressed(SDL_GameControllerGetAxis(handle, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
        const bool right =
            SDL_GameControllerGetButton(handle, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ||
            SDL_GameControllerGetButton(handle, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ||
            triggerPressed(SDL_GameControllerGetAxis(handle, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));

        // Left wins if both directions are reported at the same time. This
        // deterministic rule avoids jitter when hardware emits both edges.
        if (left) {
            return -1.0f;
        }
        if (right) {
            return 1.0f;
        }
    }

    // No GameController means there is no confirmed virtual-controller input.
    // Keyboard movement can still be handled separately by the application.
    return 0.0f;
}

// -----------------------------------------------------------------------------
bool Controls::hasDevice() const
{
    // SDL handle pointers are null when opening failed or after close().
    return m_controller != nullptr;
}

// -----------------------------------------------------------------------------
bool Controls::usesGameController() const
{
    // This is useful when showing diagnostics without exposing SDL handles to
    // the game.
    return m_controller != nullptr;
}

// -----------------------------------------------------------------------------
void Controls::registerControllerMappings()
{
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;

    for (const char* path : kControllerMappingFiles) {
        // SDL_GameControllerAddMappingsFromFile returns -1 when the file does
        // not exist or cannot be parsed. That is fine here: the SDK can still
        // continue with SDL's built-in GameController mappings.
        SDL_GameControllerAddMappingsFromFile(path);
    }
}

// -----------------------------------------------------------------------------
void Controls::loadControllerSettingFallbacks()
{
    m_buttonFallbacks = ButtonFallbacks {};
    m_fallbackDevice.clear();

    for (const char* path : kControllerSettingFiles) {
        std::ifstream file(path);
        if (!file.good()) {
            continue;
        }

        // key_mapping.ini holds one [section] per input device. The previous
        // parser ignored sections entirely and read every KEY=VALUE line, so on
        // a cabinet with both a built-in controller and an arcade control panel
        // the second section silently overwrote the first — and the two do not
        // agree on raw button numbers (B is b3 on one, b2 on the other).
        //
        // Prefer the section for the device SDL actually opened. Failing that,
        // prefer "ATG game console" (CE's virtual controller, the one that
        // carries the cabinet's own controls), then whatever came first.
        std::string wanted = m_deviceName;
        if (wanted.empty()) {
            wanted = "ATG game console";
        }

        ButtonFallbacks chosen;
        std::string chosenSection;
        ButtonFallbacks current;
        std::string section;
        bool haveChosen = false;

        auto commit = [&]() {
            if (section.empty()) {
                return;
            }
            const bool exact = (section == wanted);
            if (exact || !haveChosen) {
                chosen = current;
                chosenSection = section;
                haveChosen = true;
            }
        };

        std::string line;
        while (std::getline(file, line)) {
            const std::string trimmed = trim(line);
            if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
                commit();
                if (chosenSection == wanted && haveChosen) {
                    break;   // exact match already found; nothing can beat it
                }
                section = trimmed.substr(1, trimmed.size() - 2);
                current = ButtonFallbacks {};
                continue;
            }

            const std::size_t separator = trimmed.find('=');
            if (separator == std::string::npos) {
                continue;
            }
            const std::string key = trim(trimmed.substr(0, separator));
            const std::string value = trimmed.substr(separator + 1);

            const int button = parseSdlButtonIndex(value);
            if (button < 0) {
                continue;   // empty, or a hat/axis entry handled elsewhere
            }

            if (key == "SDL_A") {
                current.a = button;
            } else if (key == "SDL_B") {
                current.b = button;
            } else if (key == "SDL_X") {
                current.x = button;
            } else if (key == "SDL_Y") {
                current.y = button;
            } else if (key == "SDL_START") {
                current.start = button;
            } else if (key == "SDL_SELECT" || key == "SDL_BACK") {
                current.back = button;
            } else if (key == "SDL_HOME") {
                current.guide = button;
            } else if (key == "SDL_LB") {
                current.leftShoulder = button;
            } else if (key == "SDL_Z_RB") {
                current.rightShoulder = button;
            } else if (key == "SDL_LT") {
                current.leftTrigger = button;
            } else if (key == "SDL_C_RT") {
                current.rightTrigger = button;
            } else if (key == "SDL_REWIND") {
                current.rewind = button;
            } else if (key == "SDL_REWIND2") {
                current.rewind2 = button;
            }
        }
        commit();

        m_buttonFallbacks = chosen;
        m_fallbackDevice = chosenSection;
        return;
    }
}

// -----------------------------------------------------------------------------
ControlEvent Controls::eventFromMappedJoystickButton(Uint8 button) const
{
    const int i = static_cast<int>(button);

    // Ordered so the navigation controls are checked first. A device that
    // leaves a control unmapped stores -1, which can never match a real button
    // index, so no guard is needed beyond that.
    if (i == m_buttonFallbacks.a)             return ControlEvent::A;
    if (i == m_buttonFallbacks.b)             return ControlEvent::B;
    if (i == m_buttonFallbacks.x)             return ControlEvent::X;
    if (i == m_buttonFallbacks.y)             return ControlEvent::Y;
    if (i == m_buttonFallbacks.start)         return ControlEvent::Start;
    if (i == m_buttonFallbacks.back)          return ControlEvent::Back;
    if (i == m_buttonFallbacks.guide)         return ControlEvent::Guide;
    if (i == m_buttonFallbacks.leftShoulder)  return ControlEvent::LeftShoulder;
    if (i == m_buttonFallbacks.rightShoulder) return ControlEvent::RightShoulder;
    if (i == m_buttonFallbacks.leftTrigger)   return ControlEvent::LeftTrigger;
    if (i == m_buttonFallbacks.rightTrigger)  return ControlEvent::RightTrigger;
    if (i == m_buttonFallbacks.rewind)        return ControlEvent::Rewind;
    if (i == m_buttonFallbacks.rewind2)       return ControlEvent::Rewind2;

    return ControlEvent::None;
}

// The ini stores the D-pad as H1/H2/H4/H8, which are exactly SDL's hat bits.
// Without this, the fallback path had no directions at all — a cabinet SDL did
// not recognise as a GameController could not be navigated.
ControlEvent Controls::eventFromJoystickHat(Uint8 value) const
{
    switch (value) {
    case SDL_HAT_UP:    return ControlEvent::DpadUp;
    case SDL_HAT_DOWN:  return ControlEvent::DpadDown;
    case SDL_HAT_LEFT:  return ControlEvent::DpadLeft;
    case SDL_HAT_RIGHT: return ControlEvent::DpadRight;
    default:            return ControlEvent::None;   // centred or diagonal
    }
}

std::string Controls::deviceReport() const
{
    if (m_deviceReport.empty()) {
        return "No input devices found";
    }
    return m_deviceReport;
}

std::string Controls::inputSummary() const
{
    if (!m_controllers.empty()) {
        std::string names;
        for (SDL_GameController* handle : m_controllers) {
            const char* n = SDL_GameControllerName(handle);
            if (!names.empty()) {
                names += " + ";
            }
            names += n ? n : "?";
        }
        return "Controller: " + names;
    }
    if (m_buttonFallbacks.a >= 0 || m_buttonFallbacks.b >= 0) {
        return "Mapped buttons: "
             + (m_fallbackDevice.empty() ? std::string("key_mapping.ini") : m_fallbackDevice);
    }
    return "Keyboard only";
}

} // namespace AtGames
