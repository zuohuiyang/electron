// Copyright (c) 2026 Electron contributors.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

// An ordinary DLL, deliberately not a Node.js addon.
extern "C" __declspec(dllexport) int DllBlockerTestValue() {
  return 42;
}
