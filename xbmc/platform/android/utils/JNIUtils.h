/*
 *  Copyright (C) 2005-2022 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <jni.h>

namespace jni
{
class JNIUtils
{
public:
  static void SetJavaVM(JavaVM* vm);
  static JavaVM* GetJavaVM();
  static JNIEnv* GetEnv();
  static void AttachCurrentThread();
  static void DetachCurrentThread();
};
} // namespace jni
