// Copyright (c) 2024 Yang Liu.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/browser/api/electron_api_dll_blocker.h"

#include <string_view>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/chrome_elf/chrome_elf_main.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "gin/dictionary.h"
#include "shell/browser/javascript_environment.h"
#include "shell/common/gin_helper/wrappable_pointer_tags.h"
#include "shell/common/node_includes.h"
#include "v8/include/cppgc/allocation.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-cppgc.h"

namespace electron::api {

namespace {

const scoped_refptr<base::SingleThreadTaskRunner>& BlockedEventTaskRunner() {
  // Capture the runner on the UI thread before registering the callback. Keep
  // it alive through shutdown: loader callbacks can arrive after the UI exits.
  static const base::NoDestructor<scoped_refptr<base::SingleThreadTaskRunner>>
      runner(content::GetUIThreadTaskRunner({}));
  return *runner;
}

void OnDllBlocked(const char* name, size_t size) {
  BlockedEventTaskRunner()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](std::string dll_name) {
                       DllBlocker::Get()->Emit("dll-blocked", dll_name);
                     },
                     std::string(name, size)));
}

}  // namespace

const gin::WrapperInfo DllBlocker::kWrapperInfo =
    electron::MakeWrapperInfo(electron::kElectronDllBlocker);

DllBlocker::DllBlocker() = default;
DllBlocker::~DllBlocker() = default;

DllBlocker* DllBlocker::Get() {
  static base::NoDestructor<cppgc::Persistent<DllBlocker>> instance([] {
    auto* isolate = JavascriptEnvironment::GetIsolate();
    return cppgc::Persistent<DllBlocker>(
        cppgc::MakeGarbageCollected<DllBlocker>(
            isolate->GetCppHeap()->GetAllocationHandle()));
  }());
  return instance->Get();
}

void DllBlocker::StartListening() {
  BlockedEventTaskRunner();
  ::SetDllBlockCallback(OnDllBlocked);
}

const gin::WrapperInfo* DllBlocker::wrapper_info() const {
  return &kWrapperInfo;
}

const char* DllBlocker::GetHumanReadableName() const {
  return "Electron / DllBlocker";
}

gin::ObjectTemplateBuilder DllBlocker::GetObjectTemplateBuilder(
    v8::Isolate* isolate) {
  return gin_helper::EventEmitterMixin<DllBlocker>::GetObjectTemplateBuilder(
             isolate)
      .SetMethod("getState", &DllBlocker::GetState)
      .SetMethod("setState", &DllBlocker::SetState)
      .SetMethod("setBlocklist", &DllBlocker::SetBlocklist)
      .SetMethod("getCurrentBlocklist", &DllBlocker::GetCurrentBlocklist);
}

int DllBlocker::GetState() {
  int state = 0;
  ::GetDllBlockState(state);
  return state;
}

bool DllBlocker::SetState(int state) {
  // Setup states are maintained by the early loader, not by applications.
  return (state == 0 || state == 1) && ::SetDllBlockState(state);
}

bool DllBlocker::SetBlocklist(const std::vector<std::string>& blocklist) {
  std::string names;
  for (const auto& name : blocklist) {
    if (name.empty() || name.find_first_of("/\\") != std::string::npos ||
        name.find('\0') != std::string::npos) {
      return false;
    }
    names.append(name);
    names.push_back('\0');
  }
  return ::SetDllBlocklist(names.data(), names.size());
}

std::vector<std::string> DllBlocker::GetCurrentBlocklist() {
  const char* names = nullptr;
  size_t size = 0;
  std::vector<std::string> result;
  if (!::GetCurrentDllBlocklist(&names, &size)) {
    return result;
  }
  std::string_view remaining(names, size);
  while (!remaining.empty()) {
    const auto end = remaining.find('\0');
    if (end == std::string_view::npos) {
      return {};
    }
    result.emplace_back(remaining.substr(0, end));
    remaining.remove_prefix(end + 1);
  }
  return result;
}

}  // namespace electron::api

namespace {

void Initialize(v8::Local<v8::Object> exports,
                v8::Local<v8::Value> unused,
                v8::Local<v8::Context> context,
                void* priv) {
  auto* isolate = electron::JavascriptEnvironment::GetIsolate();
  gin::Dictionary dict(isolate, exports);
  dict.Set("dllBlocker", electron::api::DllBlocker::Get());
}

}  // namespace

NODE_LINKED_BINDING_CONTEXT_AWARE(electron_browser_dll_blocker, Initialize)
