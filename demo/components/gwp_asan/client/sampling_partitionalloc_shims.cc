// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/gwp_asan/client/sampling_partitionalloc_shims.h"

#include <algorithm>
#include <utility>

#include "components/crash/core/common/crash_key.h"
#include "components/gwp_asan/client/export.h"
#include "components/gwp_asan/client/guarded_page_allocator.h"
#include "components/gwp_asan/client/sampling_state.h"
#include "components/gwp_asan/common/crash_key_name.h"
#include "partition_alloc/flags.h"
#include "partition_alloc/partition_alloc.h"

#include "base/logging.h"

namespace gwp_asan {
namespace internal {

namespace {

SamplingState<PARTITIONALLOC> sampling_state;

// The global allocator singleton used by the shims. Implemented as a global
// pointer instead of a function-local static to avoid initialization checks
// for every access.
GuardedPageAllocator* gpa = nullptr;

bool AllocationHook(void** out,
                    partition_alloc::AllocFlags flags,
                    size_t size,
                    const char* type_name) {
  LOG(INFO) << "[GWP-ASan][PAHooks] AllocationHook enter size=" << size
            << " flags=" << static_cast<unsigned>(flags)
            << " type=" << (type_name ? type_name : "(null)");
  if (sampling_state.Sample(size)) [[unlikely]] {
    LOG(INFO) << "[GWP-ASan][PAHooks] Sample HIT size=" << size;
    // Ignore allocation requests with unknown flags.
    // TODO(crbug.com/40277643): Add support for memory tagging in GWP-Asan.
    constexpr auto kKnownFlags = partition_alloc::AllocFlags::kReturnNull |
                                 partition_alloc::AllocFlags::kZeroFill;
    if (!ContainsFlags(kKnownFlags, flags)) {
      // Skip if |flags| is not a subset of |kKnownFlags|.
      // i.e. if we find an unknown flag.
      LOG(INFO) << "[GWP-ASan][PAHooks] Unknown flags, skip. flags="
                << static_cast<unsigned>(flags);
      return false;
    }

    if (void* allocation = gpa->Allocate(size, 0, type_name)) {
      LOG(INFO) << "[GWP-ASan][PAHooks] GPA Allocate success ptr=" << allocation;
      *out = allocation;
      return true;
    } else {
      LOG(INFO) << "[GWP-ASan][PAHooks] GPA Allocate failed";
    }
  } else {
    LOG(INFO) << "[GWP-ASan][PAHooks] Sample MISS size=" << size;
  }
  return false;
}

bool FreeHook(void* address) {
  if (gpa->PointerIsMine(address)) [[unlikely]] {
    gpa->Deallocate(address);
    return true;
  }
  return false;
}

bool ReallocHook(size_t* out, void* address) {
  if (gpa->PointerIsMine(address)) [[unlikely]] {
    *out = gpa->GetRequestedSize(address);
    return true;
  }
  return false;
}

}  // namespace

// We expose the allocator singleton for unit tests.
GWP_ASAN_EXPORT GuardedPageAllocator& GetPartitionAllocGpaForTesting() {
  return *gpa;
}

bool InstallPartitionAllocHooks(
    const AllocatorSettings& settings,
    GuardedPageAllocator::OutOfMemoryCallback callback) {
  LOG(INFO) << "[GWP-ASan][PAHooks] Enter InstallPartitionAllocHooks "
            << "sampling_freq=" << settings.sampling_frequency
            << " min_size=" << settings.sampling_min_size
            << " max_size=" << settings.sampling_max_size
            << " total_pages=" << settings.total_pages;
  static crash_reporter::CrashKeyString<24> pa_crash_key(
      kPartitionAllocCrashKey);
  gpa = new GuardedPageAllocator();
  LOG(INFO) << "[GWP-ASan][PAHooks] GPA created ptr=" << static_cast<void*>(gpa);
  if (!gpa->Init(settings, std::move(callback), true)) {
    LOG(INFO) << "[GWP-ASan][PAHooks] GPA Init failed";
    return false;
  }
  LOG(INFO) << "[GWP-ASan][PAHooks] GPA Init success crash_key=" << gpa->GetCrashKey();
  pa_crash_key.Set(gpa->GetCrashKey());
  LOG(INFO) << "[GWP-ASan][PAHooks] CrashKey set";
  sampling_state.Init(settings.sampling_frequency);
  LOG(INFO) << "[GWP-ASan][PAHooks] SamplingState.Init freq=" << settings.sampling_frequency;
  sampling_state.SetSampleSizeRestriction(settings.sampling_min_size,
                                          settings.sampling_max_size);
  LOG(INFO) << "[GWP-ASan][PAHooks] SamplingState.SetSampleSizeRestriction min="
            << settings.sampling_min_size << " max=" << settings.sampling_max_size;
  // TODO(vtsyrklevich): Allow SetOverrideHooks to be passed in so we can hook
  // PDFium's PartitionAlloc fork.
  LOG(INFO) << "[GWP-ASan][PAHooks] Installing PartitionAlloc override hooks";
  partition_alloc::PartitionAllocHooks::SetOverrideHooks(
      &AllocationHook, &FreeHook, &ReallocHook);
  LOG(INFO) << "[GWP-ASan][PAHooks] PartitionAlloc override hooks installed";
  return true;
}

}  // namespace internal
}  // namespace gwp_asan
