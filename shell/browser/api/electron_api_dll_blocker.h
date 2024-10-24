// Copyright (c) 2024 Yang Liu.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef ELECTRON_SHELL_BROWSER_API_ELECTRON_API_DLL_BLOCKER_H_
#define ELECTRON_SHELL_BROWSER_API_ELECTRON_API_DLL_BLOCKER_H_

#include <string>
#include <vector>

#include "gin/wrappable.h"
#include "shell/browser/event_emitter_mixin.h"

namespace electron::api {

class DllBlocker final : public gin::Wrappable<DllBlocker>,
                         public gin_helper::EventEmitterMixin<DllBlocker> {
 public:
  static DllBlocker* Get();
  static void StartListening();

  static const gin::WrapperInfo kWrapperInfo;
  const gin::WrapperInfo* wrapper_info() const override;
  const char* GetHumanReadableName() const override;
  gin::ObjectTemplateBuilder GetObjectTemplateBuilder(
      v8::Isolate* isolate) override;
  const char* GetClassName() const { return "DllBlocker"; }

  DllBlocker();
  ~DllBlocker() override;
  DllBlocker(const DllBlocker&) = delete;
  DllBlocker& operator=(const DllBlocker&) = delete;

 private:
  int GetState();
  bool SetState(int state);
  bool SetBlocklist(const std::vector<std::string>& blocklist);
  std::vector<std::string> GetCurrentBlocklist();
};

}  // namespace electron::api

#endif  // ELECTRON_SHELL_BROWSER_API_ELECTRON_API_DLL_BLOCKER_H_
