/*
 *  Copyright (C) 2005-2022 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "ThreadImplAndroid.h"
#include "utils/log.h"
#include "platform/android/utils/JNIUtils.h"

#include <sys/resource.h>
#include <unistd.h>

// From
// https://developer.android.com/ndk/reference/group/thread#group___thread_1gaa6333e7531def853489b0091d846875a
#define ANDROID_PRIORITY_AUDIO -16

namespace
{
constexpr int ThreadPriorityToNativePriority(const ThreadPriority& priority)
{
  switch (priority)
  {
  case ThreadPriority::LOWEST:
    return ANDROID_PRIORITY_LOWEST;
  case ThreadPriority::BELOW_NORMAL:
    return ANDROID_PRIORITY_BACKGROUND;
  case ThreadPriority::NORMAL:
    return ANDROID_PRIORITY_DEFAULT;
  case ThreadPriority::ABOVE_NORMAL:
    return ANDROID_PRIORITY_FOREGROUND;
  case ThreadPriority::HIGHEST:
    return ANDROID_PRIORITY_AUDIO;
  default:
    return ANDROID_PRIORITY_DEFAULT;
  }
}
} // namespace

std::unique_ptr<IThreadImpl> IThreadImpl::CreateThreadImpl(std::thread::native_handle_type handle)
{
  return std::make_unique<CThreadImplAndroid>(handle);
}

CThreadImplAndroid::CThreadImplAndroid(std::thread::native_handle_type handle)
    : IThreadImpl(handle), m_threadID(gettid())
{
}

void CThreadImplAndroid::SetThreadInfo(const std::string& name)
{
  pthread_setname_np(m_handle, name.c_str());
  m_name = name;
}

bool CThreadImplAndroid::SetPriority(const ThreadPriority& priority)
{
  const int newPriority = ThreadPriorityToNativePriority(priority);
  if (setpriority(PRIO_PROCESS, m_threadID, newPriority) != 0)
  {
    CLog::Log(LOGERROR, "CThreadImplAndroid: Failed to set thread priority for thread {}", m_name);
    return false;
  }

  const int actualPriority = getpriority(PRIO_PROCESS, m_threadID);
  CLog::Log(LOGDEBUG, "[threads] name: '{}' priority: '{}'", m_name, actualPriority);

  return true;
}
