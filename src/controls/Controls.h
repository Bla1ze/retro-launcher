// Based on the controls helper (sdk/controls) from the AtGames External
// Applications SDK, https://www.atgames.net/features/external-apps, modified.
// Included with attribution to AtGames (the SDK carries no licence file).

#pragma once

// ---------------------------------------------------------------------------
// Input handling for AtGames cabinets.
//
// A cabinet can expose TWO devices at once, with the physical controls split
// between them:
//
//   "ATG game console"  CE's virtual controller   (bus 0x19, 0838:8819)
//   "HID 0838:8918"     USB arcade control panel  (bus 0x03, 0838:8918)
//
// SDL only emits events for devices it has opened, so opening just one left the
// other's buttons producing nothing at all -- no error, simply dead inputs, and
// which half died depended on enumeration order. open() therefore opens every
// recognised controller. That is safe: the two are distinct hardware, so a
// given physical button belongs to exactly one of them and no press is
// reported twice. Confirmed on hardware 2026-07-25.
//
// The key_mapping.ini fallback (used when SDL recognises no GameController)
// covers the full control set, honours [sections] -- the two devices disagree
// on raw button numbers, B being b3 on one and b2 on the other -- and handles
// hat events, without which the D-pad was unreachable.
//
// Note neither device defines "back", so ControlEvent::Back never fires on this
// hardware. Use B or Rewind.
// ---------------------------------------------------------------------------

// Shared controls helper for AtGames External Applications.
//
// Why this helper exists
// ----------------------
// A Legends control panel is visible to Linux as an input device. This helper
// deliberately uses SDL_GameController only, which is SDL's semantic controller
// layer. The firmware/CE controller mapping turns device-specific hardware
// into named virtual controls such as A, B, Start, Back, and D-pad directions.
//
// The important constraint is that this helper must not guess raw numeric
// joystick buttons. Raw indexes vary between cabinets and can make one device's
// Back button become another device's flipper or Start button. If CE has not
// exposed a semantic GameController mapping, the app should fail safely instead
// of inventing a layout.
//
// There are also two kinds of input a game normally needs:
//
//   - event(): a one-time controller event, such as pressing A or Back.
//   - horizontal(): a value read every frame while a direction is held.
//
// Keeping these concepts separate prevents one button press from firing every
// frame, while still allowing smooth continuous movement.
//
// Typical usage:
//
//   AtGames::Controls controls;
//
//   if (!controls.open()) {
//       // No controller is connected. The game may still support keyboard.
//   }
//
//   SDL_Event event;
//   while (SDL_PollEvent(&event)) {
//       switch (controls.event(event)) {
//       case AtGames::ControlEvent::A:
//           confirmSelection();
//           break;
//       case AtGames::ControlEvent::Back:
//           returnToPreviousScreen();
//           break;
//       default:
//           break;
//       }
//   }
//
//   const float movement = controls.horizontal();
//
// Call open() after SDL_INIT_GAMECONTROLLER and SDL_INIT_JOYSTICK have been
// initialized. Controls closes its SDL handles automatically.
//
// Important CE distinction
// ------------------------
// The "Start", "Dpad", and "A_function" fields in all-games.json are guide
// metadata shown by CE. They describe the game's controls to the player; they
// are not the runtime input layer used by a launched ELF.
//
// CE's own menus use cKeyMapping + XControll internally. External Applications
// cannot call that in-process API directly, so this helper mirrors the useful
// part at the SDK level: it consumes the same semantic virtual-controller
// actions after CE/SDL mapping has already normalized the physical device.
//
// The helper is also independent from the CE VirtualGamepad feature.
// VirtualGamepad is for OTG mode, where cabinet controls are sent to an
// external computer. This helper is for apps running directly on the Legends
// device.

#include <SDL.h>

#include <string>
#include <vector>

namespace AtGames {

// A small, device-independent vocabulary for controller events.
//
// The SDK exposes what the normalized SDL controller produced. It does not
// decide what a button means inside a specific game. For example, one game may
// use B as "back", while another may use B as a gameplay action.
enum class ControlEvent {
    // The SDL event is unrelated to the controls recognized by this helper.
    None,

    // Digital directional pad events.
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,

    // Face buttons exposed by SDL_GameController.
    A,
    B,
    X,
    Y,

    // System/navigation buttons exposed by SDL_GameController.
    Start,
    Back,
    Guide,

    // CE maps the cabinet REWIND control to XCNT_GAMEPAD_LEFT_THUMB. In the
    // exported SDL controller database that becomes SDL's left-stick button,
    // because SDL2 does not have a native "rewind" semantic button.
    Rewind,
    // CE also exposes REWIND2 as XCNT_GAMEPAD_RIGHT_THUMB. SDL exports that
    // through the right-stick button for the same reason.
    Rewind2,

    // Shoulder buttons and trigger press edges. The analog trigger value is
    // still available through SDL directly; this helper only reports the
    // stable "pressed" edge for simple menu/game actions.
    LeftShoulder,
    RightShoulder,
    LeftTrigger,
    RightTrigger,

    // Backward-compatible aliases used by the older sample apps. These names
    // are app-level meanings, so new code should prefer the controller names
    // above and make meaning decisions inside the game/app layer.
    Up = DpadUp,
    Down = DpadDown,
    Left = DpadLeft,
    Right = DpadRight,
    Confirm = A
};

// Owns the SDL virtual-controller input device used by an External Application.
//
// One Controls object is normally enough for a simple single-player game.
// Create it as a member of the App class so it lives for the entire game.
class Controls {
public:
    Controls() = default;

    // The destructor calls close(), so input handles are released even when
    // the application returns early because of an error.
    ~Controls();

    // SDL handles must have exactly one owner. Copying this object would make
    // two Controls instances try to close the same handle, so copying is
    // intentionally disabled.
    Controls(const Controls&) = delete;
    Controls& operator=(const Controls&) = delete;

    // Open the first SDL GameController. Returns true when CE/SDL exposes a
    // semantic virtual controller for the connected cabinet controls.
    //
    // Keyboard events still work through event() when this returns false.
    //
    // Calling open() more than once is safe. It closes the previous device
    // before searching again.
    bool open();

    // Close any controller or joystick opened by open(). Calling close() when
    // no device is open is safe.
    void close();

    // Translate one SDL event into a normalized controller event.
    //
    // SDL_QUIT is intentionally not handled here because application lifetime
    // remains the responsibility of the game's App class.
    //
    // allowJoystickAxis is kept for source compatibility with older SDK
    // samples. The virtual-controller-only helper ignores raw joystick axes.
    //
    // allowPageButtonsAsDirections is kept for source compatibility with older
    // SDK callers. It no longer changes event() output because this helper now
    // exposes controller buttons directly instead of assigning app behavior to
    // page/flipper controls.
    //
    // This function reports button-down events only. It does not repeatedly
    // return A while the button remains held.
    ControlEvent event(
        const SDL_Event& input,
        bool allowJoystickAxis = false,
        bool allowPageButtonsAsDirections = true) const;

    // Return continuous horizontal movement in the range -1.0 to 1.0.
    //
    // Only semantic GameController input is used. The helper intentionally does
    // not inspect raw joystick button numbers. D-pad and semantic flipper
    // controls are supported when the firmware exposes them through SDL.
    //
    // Return values:
    //   -1.0 = full left
    //    0.0 = centered / no input
    //    1.0 = full right
    float horizontal() const;

    // Returns true when SDL successfully opened a semantic GameController.
    bool hasDevice() const;

    // Returns true when the helper is using SDL_GameController.
    bool usesGameController() const;

    // One short line describing the live input path, for display in the UI:
    // "Controller: ATG game console", "Mapped buttons (key_mapping.ini)", or
    // "Keyboard only". Lets a user report what their cabinet is doing without
    // having to retrieve a log file.
    std::string inputSummary() const;

    // Everything SDL enumerated, comma separated, with "(unmapped)" against any
    // device that has no GameController mapping. Answers "does this cabinet even
    // expose the device we expect?" without a log file.
    std::string deviceReport() const;

private:
    // CE fallback values parsed from /userdata/key_mapping.ini. These are not
    // hard-coded raw buttons; they are the device's own exported semantic
    // mapping and are used only when SDL does not emit the matching
    // GameController button event.
    // Every control CE exports, not just the four this fallback used to read.
    // -1 means the device's ini left that control unmapped.
    struct ButtonFallbacks {
        int a = -1;
        int b = -1;
        int x = -1;
        int y = -1;
        int start = -1;
        int back = -1;
        int guide = -1;
        int leftShoulder = -1;
        int rightShoulder = -1;
        int leftTrigger = -1;
        int rightTrigger = -1;
        int rewind = -1;
        int rewind2 = -1;
    };

    // Load CE-generated SDL controller mappings when the firmware has created
    // them. This lets SDL_GameController become the preferred path on devices
    // that would otherwise appear only as raw joysticks.
    static void registerControllerMappings();

    // Name SDL reports for the opened controller, and the ini section actually
    // used for the fallback table. Both feed inputSummary().
    // Every opened controller. m_controller aliases the first and is kept so
    // the single-handle accessors below stay meaningful.
    std::vector<SDL_GameController*> m_controllers;
    std::string m_deviceName;
    std::string m_fallbackDevice;
    std::string m_deviceReport;

    // Load CE's readable key_mapping.ini fallback. Some AppStore-launched
    // contexts still expose selected cabinet buttons as joystick events even
    // after the GameController database has been registered.
    void loadControllerSettingFallbacks();

    // Convert a joystick button through the CE-exported fallback table.
    ControlEvent eventFromMappedJoystickButton(Uint8 button) const;

    // Convert a hat position into a direction. The ini stores the D-pad as
    // H1/H2/H4/H8, matching SDL_HAT_UP/RIGHT/DOWN/LEFT.
    ControlEvent eventFromJoystickHat(Uint8 value) const;

    // The pointer is non-owning in normal C++ terminology, but SDL requires
    // this class to close the handle explicitly.
    SDL_GameController* m_controller = nullptr;

    ButtonFallbacks m_buttonFallbacks;

    // Trigger axes emit many motion events while crossing their range. These
    // flags let event() return one Left/Right action per physical squeeze.
    mutable bool m_leftTriggerHeld = false;
    mutable bool m_rightTriggerHeld = false;
};

} // namespace AtGames
