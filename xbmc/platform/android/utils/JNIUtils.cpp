/*
 *  Copyright (C) 2005-2022 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JNIUtils.h"
#include "utils/log.h"

namespace jni
{
static JavaVM* g_jvm = nullptr;

void JNIUtils::SetJavaVM(JavaVM* vm)
{
  g_jvm = vm;
}

JavaVM* JNIUtils::GetJavaVM()
{
  return g_jvm;
}

JNIEnv* JNIUtils::GetEnv()
{
  if (!g_jvm)
  {
    return nullptr;
  }

  JNIEnv* env;
  if (g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
  {
    return nullptr;
  }
  return env;
}

void JNIUtils::AttachCurrentThread()
{
  if (!g_jvm)
  {
    return;
  }

  JNIEnv* env;
  if (g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK)
  {
    return; // Already attached
  }

  if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK)
  {
    CLog::Log(LOGERROR, "JNIUtils: Failed to attach current thread");
  }
}

void JNIUtils::DetachCurrentThread()
{
  if (!g_jvm)
  {
    return;
  }

  if (g_jvm->DetachCurrentThread() != JNI_OK)
  {
    CLog::Log(LOGERROR, "JNIUtils: Failed to detach current thread");
  }
}
} // namespace jni
