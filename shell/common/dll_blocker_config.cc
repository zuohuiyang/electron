// Copyright (c) 2026 Electron contributors.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/common/dll_blocker_config.h"

#include <windows.h>

#include <memory>
#include <string_view>

#include "base/compiler_specific.h"
#include "third_party/jsoncpp/source/include/json/reader.h"

namespace electron {
namespace {

constexpr char kSentinel[] = "tt-fuses-5344BEC8-BF93-1B87-2DA3-785C278AA87A";
constexpr size_t kHeaderSize = sizeof(kSentinel) - 1 + 1 + 4;
constexpr size_t kCapacity = 0x3000;
constexpr unsigned int kMaxDepth = 16;

bool ReadHexCodeUnit(std::string_view input,
                     size_t* offset,
                     unsigned int* value) {
  if (input.size() - *offset < 4)
    return false;
  *value = 0;
  for (int i = 0; i < 4; ++i) {
    const char c = input[(*offset)++];
    unsigned int digit;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    } else {
      return false;
    }
    *value = (*value << 4) | digit;
  }
  return true;
}

// Bound nesting before parsing, and reject invalid surrogate pairs that JsonCpp
// can otherwise decode as valid code points.
bool CanParseConfiguration(std::string_view input) {
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                           static_cast<int>(input.size()), nullptr, 0)) {
    return false;
  }
  unsigned int depth = 0;
  bool in_string = false;
  for (size_t i = 0; i < input.size();) {
    const char c = input[i++];
    if (!in_string) {
      if (c == '{' || c == '[') {
        if (++depth > kMaxDepth)
          return false;
      } else if (c == '}' || c == ']') {
        if (depth == 0)
          return false;
        --depth;
      } else if (c == '"') {
        in_string = true;
      }
      continue;
    }
    if (static_cast<unsigned char>(c) < 0x20)
      return false;
    if (c == '"') {
      in_string = false;
    } else if (c == '\\') {
      if (i == input.size())
        return false;
      if (input[i++] != 'u')
        continue;
      unsigned int code_unit;
      if (!ReadHexCodeUnit(input, &i, &code_unit) || code_unit == 0)
        return false;
      if (code_unit >= 0xdc00 && code_unit <= 0xdfff)
        return false;
      if (code_unit >= 0xd800 && code_unit <= 0xdbff) {
        if (input.size() - i < 6 || input[i++] != '\\' || input[i++] != 'u')
          return false;
        if (!ReadHexCodeUnit(input, &i, &code_unit) || code_unit < 0xdc00 ||
            code_unit > 0xdfff) {
          return false;
        }
      }
    }
  }
  return depth == 0 && !in_string;
}

}  // namespace

std::wstring GetDllBlockerRegistryPath() {
  HMODULE executable = GetModuleHandleW(nullptr);
  if (!executable)
    return {};
  using GetWire = const char* (*)();
  const auto get_wire = reinterpret_cast<GetWire>(
      GetProcAddress(executable, "TtFusesGetFuseWire"));
  if (!get_wire)
    return {};
  const char* wire = get_wire();
  if (!wire)
    return {};
  // SAFETY: The executable's export returns the statically allocated wire in
  // dll_blocker_config_wire.cc, whose size is fixed independently of its data.
  const auto wire_view =
      UNSAFE_BUFFERS(std::string_view(wire, kHeaderSize + kCapacity));
  constexpr char kFormat[] = {1, 0, 0, 0x30, 0};
  if (!wire_view.starts_with(kSentinel) ||
      wire_view.substr(sizeof(kSentinel) - 1, sizeof(kFormat)) !=
          std::string_view(kFormat, sizeof(kFormat))) {
    return {};
  }
  std::string_view data = wire_view.substr(kHeaderSize);
  const size_t end = data.find('\0');
  if (end == std::string_view::npos)
    return {};
  data = data.substr(0, end);
  if (!CanParseConfiguration(data))
    return {};

  Json::CharReaderBuilder builder;
  Json::CharReaderBuilder::strictMode(&builder.settings_);
  builder["collectComments"] = false;
  builder["stackLimit"] = kMaxDepth + 2;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value config;
  if (!reader->parse(data.data(), std::to_address(data.end()), &config,
                     nullptr) ||
      !config.isObject())
    return {};
  const Json::Value& field = config["registryPath"];
  if (!field.isString())
    return {};
  const std::string path = field.asString();
  if (path.find('\0') != std::string::npos)
    return {};
  const int length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                          static_cast<int>(path.size()), nullptr, 0);
  if (length <= 0)
    return {};
  std::wstring result(length, L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                          static_cast<int>(path.size()), result.data(),
                          length) != length) {
    return {};
  }
  constexpr std::wstring_view kSoftware = L"Software\\";
  if (result.size() <= kSoftware.size() || result.back() == L'\\' ||
      result.find(L"\\\\") != std::wstring::npos ||
      CompareStringOrdinal(result.data(), static_cast<int>(kSoftware.size()),
                           kSoftware.data(), static_cast<int>(kSoftware.size()),
                           TRUE) != CSTR_EQUAL) {
    return {};
  }
  return result;
}

}  // namespace electron
