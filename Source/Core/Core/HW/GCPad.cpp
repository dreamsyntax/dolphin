// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/GCPad.h"

#include <cstring>

#include "Common/Common.h"
#include "Core/HW/GCPadEmu.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/GCPadStatus.h"
#include "InputCommon/InputConfig.h"

namespace Pad
{
static InputConfig s_config("GCPadNew", _trans("Pad"), "GCPad", "Pad");
InputConfig* GetConfig()
{
  return &s_config;
}
const static int vibrationHIS_SIZE = 3;
std::array<double, vibrationHIS_SIZE> vibrationHistory;


void Shutdown()
{
  s_config.UnregisterHotplugCallback();

  s_config.ClearControllers();
}

void Initialize()
{
  if (s_config.ControllersNeedToBeCreated())
  {
    for (unsigned int i = 0; i < 4; ++i)
      s_config.CreateController<GCPad>(i);
  }

  s_config.RegisterHotplugCallback();

  // Load the saved controller config
  s_config.LoadConfig();
}

void LoadConfig()
{
  s_config.LoadConfig();
}

void GenerateDynamicInputTextures()
{
  s_config.GenerateControllerTextures();
}

bool IsInitialized()
{
  return !s_config.ControllersNeedToBeCreated();
}

GCPadStatus GetStatus(int pad_num)
{
  return static_cast<GCPad*>(s_config.GetController(pad_num))->GetInput();
}

ControllerEmu::ControlGroup* GetGroup(int pad_num, PadGroup group)
{
  return static_cast<GCPad*>(s_config.GetController(pad_num))->GetGroup(group);
}

void addFirst(double* arr, double newFloat)
{
  // Move existing elements back one position
  // Start from the second to last element and move towards the beginning
  for (int i = vibrationHIS_SIZE - 2; i >= 0; --i)
  {
    arr[i + 1] = arr[i];
  }

  // Add the new float to the first position
  arr[0] = newFloat;
}

void Rumble(const int pad_num, const ControlState strength)
{
  addFirst(vibrationHistory.data(), strength);
  double sum = 0.0f;
  for (int i = 0; i < vibrationHIS_SIZE; ++i)
  {
    sum += vibrationHistory[i];
  }
  double average = sum / vibrationHIS_SIZE;
  static_cast<GCPad*>(s_config.GetController(pad_num))->SetOutput(average);
}

void ResetRumble(const int pad_num)
{
  addFirst(vibrationHistory.data(), 0.0f);
  double sum = 0.0f;
  for (int i = 0; i < vibrationHIS_SIZE; ++i)
  {
    sum += vibrationHistory[i];
  }
  double average = sum / vibrationHIS_SIZE;
  static_cast<GCPad*>(s_config.GetController(pad_num))->SetOutput(average);
}

bool GetMicButton(const int pad_num)
{
  return static_cast<GCPad*>(s_config.GetController(pad_num))->GetMicButton();
}
}  // namespace Pad
