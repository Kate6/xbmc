/*
 *  Copyright (C) 2025 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "settings/lib/ISettingCallback.h"
#include <memory>

class CSetting;

namespace KODI
{
namespace AE
{
namespace SINK
{
class CAESinkPipewireSettings : public ISettingCallback
{
public:
  static CAESinkPipewireSettings& GetInstance();

  CAESinkPipewireSettings(const CAESinkPipewireSettings&) = delete;
  CAESinkPipewireSettings& operator=(const CAESinkPipewireSettings&) = delete;

  // ISettingCallback implementation
  bool OnSettingChanging(const std::shared_ptr<const CSetting>& setting) override;
  void OnSettingChanged(const std::shared_ptr<const CSetting>& setting) override;

protected:
  CAESinkPipewireSettings() = default;
};

} // namespace SINK
} // namespace AE
} // namespace KODI
