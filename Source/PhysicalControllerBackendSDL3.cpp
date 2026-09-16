/***************************************************************************************************
 * Xidi
 *   DirectInput interface for XInput controllers.
 ***************************************************************************************************
 * Authored by Samuel Grossman
 * Copyright (c) 2016-2026
 ***********************************************************************************************//**
 * @file PhysicalControllerBackendSDL3.cpp
 *   Implementation of an SDL3-based physical controller backend.
 **************************************************************************************************/

#include "PhysicalControllerBackendSDL3.h"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string_view>

#include "PhysicalControllerTypes.h"

namespace Xidi
{
  using namespace ::Xidi::Controller;

  namespace
  {
    /// Maximum number of controllers this backend will expose. SDL itself has no such limit, but
    /// the rest of Xidi is built around a small fixed number of physical controller slots, so this
    /// is kept at the XInput value. Raising it only requires that the rest of Xidi agree.
    static constexpr TPhysicalControllerIndex kMaxControllerCount = 4;

    /// Duration, in milliseconds, requested for each rumble command. SDL rumble effects expire on
    /// their own, so the requested duration simply needs to comfortably outlast the interval
    /// between force feedback state writes. Each write restarts the effect.
    static constexpr Uint32 kRumbleDurationMilliseconds = 2000;

    /// Full-scale value of an SDL gamepad axis in the positive direction.
    static constexpr int32_t kSdlAxisMax = 32767;

    /// Mapping from SDL gamepad buttons to Xidi physical buttons. Order within this table is
    /// irrelevant; the bit positions come from the Xidi enumerator.
    static constexpr struct
    {
      SDL_GamepadButton sdlButton;
      EPhysicalButton physicalButton;
    } kButtonMap[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, EPhysicalButton::DpadUp},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, EPhysicalButton::DpadDown},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, EPhysicalButton::DpadLeft},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, EPhysicalButton::DpadRight},
        {SDL_GAMEPAD_BUTTON_START, EPhysicalButton::Start},
        {SDL_GAMEPAD_BUTTON_BACK, EPhysicalButton::Back},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, EPhysicalButton::LS},
        {SDL_GAMEPAD_BUTTON_RIGHT_STICK, EPhysicalButton::RS},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, EPhysicalButton::LB},
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, EPhysicalButton::RB},
        {SDL_GAMEPAD_BUTTON_SOUTH, EPhysicalButton::A},
        {SDL_GAMEPAD_BUTTON_EAST, EPhysicalButton::B},
        {SDL_GAMEPAD_BUTTON_WEST, EPhysicalButton::X},
        {SDL_GAMEPAD_BUTTON_NORTH, EPhysicalButton::Y},
    };

    /// Negates an SDL axis reading without overflowing. SDL reports positive values for down and
    /// right on both sticks, whereas Xidi, following XInput, expects positive for up.
    static inline int16_t InvertAxis(Sint16 axisValue)
    {
      if (INT16_MIN == axisValue) return INT16_MAX;
      return static_cast<int16_t>(-axisValue);
    }

    /// Converts an SDL trigger axis reading, which is unidirectional and full-scale at 32767, into
    /// the 8-bit unidirectional form that Xidi expects from XInput.
    static inline uint8_t ConvertTrigger(Sint16 axisValue)
    {
      if (axisValue <= 0) return 0;
      return static_cast<uint8_t>((static_cast<int32_t>(axisValue) * 255) / kSdlAxisMax);
    }

    /// Case-insensitive search for a wide-character substring.
    static const wchar_t* FindSubstringCaseInsensitive(const wchar_t* haystack, const wchar_t* needle)
    {
      if ((nullptr == haystack) || (nullptr == needle) || (L'\0' == *needle)) return nullptr;

      for (const wchar_t* base = haystack; L'\0' != *base; ++base)
      {
        const wchar_t* h = base;
        const wchar_t* n = needle;

        while ((L'\0' != *h) && (L'\0' != *n) && (towlower(*h) == towlower(*n)))
        {
          ++h;
          ++n;
        }

        if (L'\0' == *n) return base;
      }

      return nullptr;
    }

    /// Reads a fixed-length hexadecimal number from a string. Returns -1 on any parse failure.
    static int32_t ParseHexDigits(const wchar_t* str, unsigned int numDigits)
    {
      int32_t value = 0;

      for (unsigned int i = 0; i < numDigits; ++i)
      {
        const wchar_t c = str[i];
        int32_t digit = 0;

        if ((c >= L'0') && (c <= L'9'))
          digit = c - L'0';
        else if ((c >= L'a') && (c <= L'f'))
          digit = 10 + (c - L'a');
        else if ((c >= L'A') && (c <= L'F'))
          digit = 10 + (c - L'A');
        else
          return -1;

        value = (value << 4) | digit;
      }

      return value;
    }

    /// Attempts to extract a USB vendor and product ID pair from a DirectInput GUID and device path
    /// string. Two forms are recognized: the "VID_xxxx" and "PID_xxxx" fields present in most device
    /// interface paths, and the DirectInput product GUID, whose first field encodes the product ID
    /// in its upper half and the vendor ID in its lower half. Returns false if neither is present.
    static bool ParseVendorAndProductId(
        const wchar_t* guidAndPath, uint16_t& vendorId, uint16_t& productId)
    {
      const wchar_t* vidField = FindSubstringCaseInsensitive(guidAndPath, L"VID_");
      const wchar_t* pidField = FindSubstringCaseInsensitive(guidAndPath, L"PID_");

      if ((nullptr != vidField) && (nullptr != pidField))
      {
        const int32_t vid = ParseHexDigits(&vidField[4], 4);
        const int32_t pid = ParseHexDigits(&pidField[4], 4);

        if ((vid >= 0) && (pid >= 0))
        {
          vendorId = static_cast<uint16_t>(vid);
          productId = static_cast<uint16_t>(pid);
          return true;
        }
      }

      // Fall back to the leading product GUID, which for HID devices looks like
      // {pppp-vvvv-0000-0000-0000-504944564944}, packed into the first GUID field.
      const wchar_t* guidStart = guidAndPath;
      if (L'{' == *guidStart) ++guidStart;

      const int32_t guidFirstField = ParseHexDigits(guidStart, 8);
      if (guidFirstField >= 0)
      {
        vendorId = static_cast<uint16_t>(guidFirstField & 0xffff);
        productId = static_cast<uint16_t>((guidFirstField >> 16) & 0xffff);
        return true;
      }

      return false;
    }

    /// Owns the set of open SDL gamepads and the assignment of those gamepads to Xidi physical
    /// controller slots. Once a gamepad is assigned to a slot it stays there until it disconnects,
    /// and a freed slot is reused by the next gamepad to arrive.
    class ControllerSet
    {
    public:

      /// Brings the slot assignments up to date with the devices SDL currently sees. Safe to call
      /// on every poll; it is cheap when nothing has changed.
      void Refresh(void)
      {
        std::scoped_lock lock(mutex);

        for (auto& controller : controllers)
        {
          if ((nullptr != controller) && (false == SDL_GamepadConnected(controller)))
          {
            SDL_CloseGamepad(controller);
            controller = nullptr;
          }
        }

        int numJoysticks = 0;
        SDL_JoystickID* const joystickIds = SDL_GetJoysticks(&numJoysticks);
        if (nullptr == joystickIds) return;

        for (int i = 0; i < numJoysticks; ++i)
        {
          const SDL_JoystickID instanceId = joystickIds[i];
          if (false == SDL_IsGamepad(instanceId)) continue;
          if (IsAlreadyAssigned(instanceId)) continue;

          const int freeSlot = FindFreeSlot();
          if (freeSlot < 0) break;

          SDL_Gamepad* const controller = SDL_OpenGamepad(instanceId);
          if (nullptr == controller) continue;

          controllers[freeSlot] = controller;
          SDL_SetGamepadPlayerIndex(controller, freeSlot);
        }

        SDL_free(joystickIds);
      }

      /// Retrieves the gamepad occupying the specified slot, or `nullptr` if the slot is empty.
      SDL_Gamepad* GetController(TPhysicalControllerIndex physicalControllerIndex)
      {
        if (physicalControllerIndex >= kMaxControllerCount) return nullptr;

        std::scoped_lock lock(mutex);
        return controllers[physicalControllerIndex];
      }

      /// Determines whether any currently-known device matches the specified vendor and product
      /// identifiers. Used to decide whether a DirectInput device is one this backend handles.
      bool HasDeviceWithVendorAndProductId(uint16_t vendorId, uint16_t productId)
      {
        std::scoped_lock lock(mutex);

        int numJoysticks = 0;
        SDL_JoystickID* const joystickIds = SDL_GetJoysticks(&numJoysticks);
        if (nullptr == joystickIds) return false;

        bool found = false;

        for (int i = 0; i < numJoysticks; ++i)
        {
          const SDL_JoystickID instanceId = joystickIds[i];
          if (false == SDL_IsGamepad(instanceId)) continue;

          if ((vendorId == SDL_GetJoystickVendorForID(instanceId)) &&
              (productId == SDL_GetJoystickProductForID(instanceId)))
          {
            found = true;
            break;
          }
        }

        SDL_free(joystickIds);
        return found;
      }

    private:

      bool IsAlreadyAssigned(SDL_JoystickID instanceId) const
      {
        for (SDL_Gamepad* const controller : controllers)
        {
          if (nullptr == controller) continue;

          SDL_Joystick* const joystick = SDL_GetGamepadJoystick(controller);
          if ((nullptr != joystick) && (instanceId == SDL_GetJoystickID(joystick))) return true;
        }

        return false;
      }

      int FindFreeSlot(void) const
      {
        for (int i = 0; i < static_cast<int>(controllers.size()); ++i)
          if (nullptr == controllers[i]) return i;

        return -1;
      }

      std::array<SDL_Gamepad*, kMaxControllerCount> controllers = {};

      std::mutex mutex;
    };

    /// Singleton set of SDL gamepads managed by this backend.
    static ControllerSet& GetControllerSet(void)
    {
      static ControllerSet controllerSet;
      return controllerSet;
    }

    /// Whether or not SDL initialization succeeded. Reads and writes of physical controller state
    /// are no-ops if it did not.
    static bool sdlIsInitialized = false;
  } // namespace

  std::wstring_view PhysicalControllerBackendSDL3::PluginName(void)
  {
    return L"SDL3";
  }

  bool PhysicalControllerBackendSDL3::Initialize(void)
  {
    if (true == sdlIsInitialized) return true;

    // Xidi runs inside the game process and polls from a background thread, so SDL must be willing
    // to report input while the game window does not have focus and must not require an event pump.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

    if (false == SDL_InitSubSystem(SDL_INIT_GAMEPAD)) return false;

    // State is obtained by explicitly calling SDL_UpdateGamepads rather than by draining the event
    // queue, which keeps this backend from interfering with a host application that might be using
    // SDL for its own purposes.
    SDL_SetGamepadEventsEnabled(false);

    // Optional community mapping database. Absence of the file is not an error; SDL ships with a
    // built-in mapping table that covers common controllers.
    SDL_AddGamepadMappingsFromFile("gamecontrollerdb.txt");

    sdlIsInitialized = true;
    GetControllerSet().Refresh();
    return true;
  }

  TPhysicalControllerIndex PhysicalControllerBackendSDL3::MaxPhysicalControllerCount(void)
  {
    return kMaxControllerCount;
  }

  bool PhysicalControllerBackendSDL3::SupportsControllerByGuidAndPath(const wchar_t* guidAndPath)
  {
    if ((false == sdlIsInitialized) || (nullptr == guidAndPath)) return false;

    uint16_t vendorId = 0;
    uint16_t productId = 0;

    if (false == ParseVendorAndProductId(guidAndPath, vendorId, productId)) return false;
    if ((0 == vendorId) && (0 == productId)) return false;

    return GetControllerSet().HasDeviceWithVendorAndProductId(vendorId, productId);
  }

  SPhysicalControllerCapabilities PhysicalControllerBackendSDL3::GetCapabilities(void)
  {
    return {
        .stick = kPhysicalCapabilitiesAllAnalogSticks,
        .trigger = kPhysicalCapabilitiesAllAnalogTriggers,
        .button = kPhysicalCapabilitiesStandardXInputButtons,
        .forceFeedbackActuator = kPhysicalCapabilitiesStandardXInputForceFeedbackActuators};
  }

  SPhysicalControllerState PhysicalControllerBackendSDL3::ReadInputState(
      TPhysicalControllerIndex physicalControllerIndex)
  {
    if (false == sdlIsInitialized) return {.deviceStatus = EPhysicalDeviceStatus::Error};

    SDL_UpdateGamepads();
    GetControllerSet().Refresh();

    SDL_Gamepad* const controller = GetControllerSet().GetController(physicalControllerIndex);
    if (nullptr == controller) return {.deviceStatus = EPhysicalDeviceStatus::NotConnected};

    // Guide and Share are deliberately left out. The capabilities reported above are the standard
    // XInput set, which does not include them, and reporting a button outside of the advertised
    // capabilities would confuse the mapping layer.
    uint16_t buttonState = 0;

    for (const auto& buttonMapping : kButtonMap)
    {
      if (true == SDL_GetGamepadButton(controller, buttonMapping.sdlButton))
        buttonState |= static_cast<uint16_t>(
            1u << static_cast<unsigned int>(buttonMapping.physicalButton));
    }

    return {
        .deviceStatus = EPhysicalDeviceStatus::Ok,
        .stick =
            {SDL_GetGamepadAxis(controller, SDL_GAMEPAD_AXIS_LEFTX),
             InvertAxis(SDL_GetGamepadAxis(controller, SDL_GAMEPAD_AXIS_LEFTY)),
             SDL_GetGamepadAxis(controller, SDL_GAMEPAD_AXIS_RIGHTX),
             InvertAxis(SDL_GetGamepadAxis(controller, SDL_GAMEPAD_AXIS_RIGHTY))},
        .trigger =
            {ConvertTrigger(SDL_GetGamepadAxis(controller, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)),
             ConvertTrigger(
                 SDL_GetGamepadAxis(controller, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))},
        .button = buttonState};
  }

  bool PhysicalControllerBackendSDL3::WriteForceFeedbackState(
      TPhysicalControllerIndex physicalControllerIndex, SPhysicalControllerVibration vibrationState)
  {
    if (false == sdlIsInitialized) return false;

    SDL_Gamepad* const controller = GetControllerSet().GetController(physicalControllerIndex);
    if (nullptr == controller) return false;

    return SDL_RumbleGamepad(
        controller,
        vibrationState.leftMotor,
        vibrationState.rightMotor,
        kRumbleDurationMilliseconds);
  }
} // namespace Xidi
