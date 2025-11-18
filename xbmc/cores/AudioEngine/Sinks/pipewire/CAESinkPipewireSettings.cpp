/*
 *  Copyright (C) 2025 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "CAESinkPipewireSettings.h"
#include "AESinkPipewire.h" // To access Register() and Destroy()
#include "settings/lib/Setting.h"
#include "utils/log.h"
#include "utils/StringUtils.h" // For StringUtils::EqualsNoCase

namespace KODI
{
namespace AE
{
namespace SINK
{

CAESinkPipewireSettings& CAESinkPipewireSettings::GetInstance()
{
  static CAESinkPipewireSettings instance;
  return instance;
}

bool CAESinkPipewireSettings::OnSettingChanging(const std::shared_ptr<const CSetting>& setting)
{
  // Allow changes to proceed
  return true;
}

void CAESinkPipewireSettings::OnSettingChanged(const std::shared_ptr<const CSetting>& setting)
{
}

} // namespace SINK
} // namespace AE
} // namespace KODI
