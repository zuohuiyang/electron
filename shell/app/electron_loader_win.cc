// Copyright (c) 2026 Electron contributors.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include <windows.h>  // windows.h must be included first

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "content/public/app/sandbox_helper_win.h"
#include "electron/fuses.h"
#include "sandbox/win/src/sandbox_types.h"

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, wchar_t* cmd, int) {
  base::CommandLine::Init(0, nullptr);

  base::FilePath executable_dir;
  if (!base::PathService::Get(base::DIR_EXE, &executable_dir))
    return ERROR_PATH_NOT_FOUND;
  const base::FilePath runtime_path =
      executable_dir.Append(FILE_PATH_LITERAL("main.dll"));

  sandbox::SandboxInterfaceInfo sandbox_info = {nullptr};
  content::InitializeSandboxInfo(&sandbox_info);

  // Only preread browser launches. Conservatively skip Node launches even when
  // their RunAsNode fuse may have been disabled in the executable.
  if (base::CommandLine::ForCurrentProcess()
          ->GetSwitchValueASCII("type")
          .empty() &&
      !base::Environment::Create()->HasVar("ELECTRON_RUN_AS_NODE")) {
    // A failed preread must not prevent the normal DLL load below.
    base::PreReadFile(runtime_path, /*is_executable=*/true,
                      /*sequential=*/false);
  }

  // Keep the runtime loaded through CRT shutdown, including addon destructors.
  HMODULE runtime = ::LoadLibraryExW(
      runtime_path.value().c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!runtime) {
    DWORD error = ::GetLastError();
    PLOG(ERROR) << "Unable to load Electron runtime";
    return static_cast<int>(error);
  }

  using MainEntry = int (*)(HINSTANCE, wchar_t*, sandbox::SandboxInterfaceInfo*,
                            const volatile char*);
  auto main_entry =
      reinterpret_cast<MainEntry>(::GetProcAddress(runtime, "ElectronMain"));
  if (!main_entry) {
    DWORD error = ::GetLastError();
    PLOG(ERROR) << "Unable to find Electron runtime entry point";
    return static_cast<int>(error);
  }
  return main_entry(instance, cmd, &sandbox_info, electron::fuses::kFuseWire);
}
