// Copyright (c) 2026 Electron contributors.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef ELECTRON_SHELL_COMMON_DLL_BLOCKER_CONFIG_H_
#define ELECTRON_SHELL_COMMON_DLL_BLOCKER_CONFIG_H_

#include <string>

namespace electron {

// Reads the registryPath field of the executable's version-1 configuration
// wire. An absent or invalid configuration disables DLL blocking.
std::wstring GetDllBlockerRegistryPath();

}  // namespace electron

#endif  // ELECTRON_SHELL_COMMON_DLL_BLOCKER_CONFIG_H_
