// Copyright (c) 2026 Electron contributors.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

namespace {

// Keep the original version-1 wire format so existing packaging tools can
// configure registryPath. The four-byte capacity is stored in big-endian order.
// No C++ initialization may be required before electron_elf's DllMain reads it.
const char kConfigurationWire[45 + 1 + 4 + 0x3000] =
    "tt-fuses-5344BEC8-BF93-1B87-2DA3-785C278AA87A"
    "\x01\x00\x00\x30\x00";

}  // namespace

extern "C" __declspec(dllexport) const char* TtFusesGetFuseWire() {
  return kConfigurationWire;
}
