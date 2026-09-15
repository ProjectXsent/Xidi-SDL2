/***************************************************************************************************
 * Xidi
 *   DirectInput interface for XInput controllers.
 ***************************************************************************************************
 * Authored by Samuel Grossman
 * Copyright (c) 2016-2026
 ***********************************************************************************************//**
 * @file PhysicalControllerBackendSDL2.h
 *   Declaration of the SDL2 physical controller backend.
 **************************************************************************************************/

#pragma once

#include "PhysicalControllerBackend.h"
#include "PluginTypes.h"

namespace Xidi
{
  /// Implements an SDL2-based backend for communicating with physical controllers.
  class PhysicalControllerBackendSDL2 : public IPhysicalControllerBackend
  {
  public:

    // IPlugin
    std::wstring_view PluginName(void) override;
    bool Initialize(void) override;

    // IPhysicalControllerBackend
    TPhysicalControllerIndex MaxPhysicalControllerCount(void) override;
    bool SupportsControllerByGuidAndPath(const wchar_t* guidAndPath) override;
    SPhysicalControllerCapabilities GetCapabilities(void) override;
    SPhysicalControllerState ReadInputState(
        TPhysicalControllerIndex physicalControllerIndex) override;
    bool WriteForceFeedbackState(
        TPhysicalControllerIndex physicalControllerIndex,
        SPhysicalControllerVibration vibrationState) override;
  };
} // namespace Xidi
