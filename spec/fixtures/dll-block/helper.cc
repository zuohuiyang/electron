// Copyright (c) 2026 Electron contributors.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include <windows.h>

#include <tlhelp32.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>

#include "base/compiler_specific.h"

namespace {

constexpr DWORD kTimeout = 30000;

[[noreturn]] void Fail(std::string_view operation) {
  fprintf(stderr, "%.*s failed: %lu\n", static_cast<int>(operation.size()),
          operation.data(), GetLastError());
  ExitProcess(2);
}

uintptr_t FindModule(DWORD pid, const wchar_t* name) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
  if (snapshot == INVALID_HANDLE_VALUE)
    Fail("CreateToolhelp32Snapshot");
  MODULEENTRY32W entry = {sizeof(entry)};
  uintptr_t result = 0;
  if (Module32FirstW(snapshot, &entry)) {
    do {
      if (!_wcsicmp(entry.szModule, name)) {
        result = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
        break;
      }
    } while (Module32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return result;
}

bool Inject(HANDLE process, DWORD pid, const wchar_t* dll) {
  const auto local_load =
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
  HMODULE owner = nullptr;
  if (!local_load ||
      !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(local_load), &owner)) {
    Fail("GetModuleHandleExW");
  }
  wchar_t owner_path[MAX_PATH];
  const DWORD owner_length = GetModuleFileNameW(owner, owner_path, MAX_PATH);
  if (!owner_length || owner_length == MAX_PATH)
    Fail("GetModuleFileNameW");
  const std::wstring_view owner_name(owner_path, owner_length);
  const auto basename = owner_name.find_last_of(L'\\');
  const uintptr_t remote_owner = FindModule(
      pid,
      owner_name.substr(basename == std::wstring_view::npos ? 0 : basename + 1)
          .data());
  if (!remote_owner)
    Fail("FindModule(loader)");
  const uintptr_t remote_load = remote_owner +
                                reinterpret_cast<uintptr_t>(local_load) -
                                reinterpret_cast<uintptr_t>(owner);
  const size_t bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
  void* argument = VirtualAllocEx(process, nullptr, bytes,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!argument || !WriteProcessMemory(process, argument, dll, bytes, nullptr))
    Fail("WriteProcessMemory");
  HANDLE thread =
      CreateRemoteThread(process, nullptr, 0,
                         reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_load),
                         argument, 0, nullptr);
  if (!thread || WaitForSingleObject(thread, kTimeout) != WAIT_OBJECT_0)
    Fail("remote LoadLibraryW");
  CloseHandle(thread);
  VirtualFreeEx(process, argument, 0, MEM_RELEASE);
  const std::wstring_view dll_path(dll);
  const auto dll_name = dll_path.find_last_of(L'\\');
  // Do not truncate the HMODULE to the 32-bit remote thread exit code.
  return FindModule(
             pid,
             dll_path
                 .substr(dll_name == std::wstring_view::npos ? 0 : dll_name + 1)
                 .data()) != 0;
}

std::wstring Quote(const wchar_t* value) {
  // Paths used by this fixture are files, so cannot end in a backslash.
  return L"\"" + std::wstring(value) + L"\"";
}

int Early(const wchar_t* exe,
          const wchar_t* script,
          const wchar_t* dll,
          const wchar_t* pipe) {
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                       &limits, sizeof(limits))) {
    Fail("CreateJobObjectW");
  }
  std::wstring command = Quote(exe) + L" " + Quote(script) + L" " + Quote(pipe);
  STARTUPINFOW startup = {sizeof(startup)};
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION child = {};
  if (!CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE,
                      DEBUG_ONLY_THIS_PROCESS | CREATE_SUSPENDED, nullptr,
                      nullptr, &startup, &child)) {
    Fail("CreateProcessW");
  }
  if (!AssignProcessToJobObject(job, child.hProcess))
    Fail("AssignProcessToJobObject");
  ResumeThread(child.hThread);
  // Break at the actual EXE entry point, rather than relying on the loader's
  // initial debugger breakpoint being after every static DLL's DllMain.
  void* entry_point = nullptr;
#if defined(_M_ARM64)
  const DWORD breakpoint = 0xd4200000;  // BRK #0
#else
  const BYTE breakpoint = 0xcc;  // INT 3
#endif
  BYTE original[sizeof(breakpoint)] = {};
  for (;;) {
    DEBUG_EVENT event = {};
    if (!WaitForDebugEvent(&event, kTimeout))
      Fail("WaitForDebugEvent");
    DWORD continuation = DBG_CONTINUE;
    bool at_entry = false;
    if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
      entry_point =
          reinterpret_cast<void*>(event.u.CreateProcessInfo.lpStartAddress);
      if (!ReadProcessMemory(child.hProcess, entry_point, original,
                             sizeof(original), nullptr) ||
          !WriteProcessMemory(child.hProcess, entry_point, &breakpoint,
                              sizeof(breakpoint), nullptr) ||
          !FlushInstructionCache(child.hProcess, entry_point,
                                 sizeof(breakpoint))) {
        Fail("install entry breakpoint");
      }
      if (event.u.CreateProcessInfo.hFile)
        CloseHandle(event.u.CreateProcessInfo.hFile);
    } else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
      if (event.u.LoadDll.hFile)
        CloseHandle(event.u.LoadDll.hFile);
    } else if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
      const auto& exception = event.u.Exception.ExceptionRecord;
      at_entry = exception.ExceptionCode == EXCEPTION_BREAKPOINT &&
                 exception.ExceptionAddress == entry_point;
      if (at_entry) {
        CONTEXT context = {};
        context.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(child.hThread, &context))
          Fail("GetThreadContext");
#if defined(_M_ARM64)
        context.Pc = reinterpret_cast<DWORD64>(entry_point);
#elif defined(_M_X64)
        context.Rip = reinterpret_cast<DWORD64>(entry_point);
#else
        context.Eip = reinterpret_cast<DWORD>(entry_point);
#endif
        if (!WriteProcessMemory(child.hProcess, entry_point, original,
                                sizeof(original), nullptr) ||
            !FlushInstructionCache(child.hProcess, entry_point,
                                   sizeof(original)) ||
            !SetThreadContext(child.hThread, &context)) {
          Fail("restore entry point");
        }
      } else if (exception.ExceptionCode != EXCEPTION_BREAKPOINT) {
        continuation = DBG_EXCEPTION_NOT_HANDLED;
      }
    } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
      Fail("child exited before entry point");
    }
    if (at_entry && SuspendThread(child.hThread) == DWORD(-1))
      Fail("SuspendThread");
    if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continuation))
      Fail("ContinueDebugEvent");
    if (at_entry)
      break;
  }
  if (!DebugActiveProcessStop(child.dwProcessId))
    Fail("DebugActiveProcessStop");
  const bool loaded = Inject(child.hProcess, child.dwProcessId, dll);
  printf("@@DLLBLOCKER@@{\"earlyLoaded\":%s}\n", loaded ? "true" : "false");
  fflush(stdout);
  if (ResumeThread(child.hThread) == DWORD(-1))
    Fail("ResumeThread");
  if (WaitForSingleObject(child.hProcess, kTimeout) != WAIT_OBJECT_0)
    Fail("child exit");
  DWORD exit_code = 2;
  GetExitCodeProcess(child.hProcess, &exit_code);
  CloseHandle(child.hThread);
  CloseHandle(child.hProcess);
  CloseHandle(job);
  return static_cast<int>(exit_code);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  // SAFETY: The CRT supplies argc valid argument pointers to wmain.
  const auto args = UNSAFE_BUFFERS(std::span(argv, static_cast<size_t>(argc)));
  if (args.size() == 6 && std::wstring_view(args[1]) == L"--early")
    return Early(args[2], args[3], args[4], args[5]);
  if (args.size() != 4 || std::wstring_view(args[1]) != L"--inject")
    return 2;
  // SAFETY: The CRT guarantees that each argument is null-terminated.
  const DWORD pid = UNSAFE_BUFFERS(wcstoul(args[2], nullptr, 10));
  HANDLE process =
      OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                      PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                  FALSE, pid);
  if (!process)
    Fail("OpenProcess");
  const bool loaded = Inject(process, pid, args[3]);
  CloseHandle(process);
  printf("%s\n", loaded ? "true" : "false");
  return 0;
}
