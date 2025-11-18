/*
 *  Copyright (C) 2005-2022 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "threads/IThreadImpl.h"

class CThreadImplAndroid : public IThreadImpl
{
public:
  CThreadImplAndroid(std::thread::native_handle_type handle);

  void SetThreadInfo(const std::string& name) override;
  bool SetPriority(const ThreadPriority& priority) override;

private:
  std::string m_name;
  pid_t m_threadID;
};
