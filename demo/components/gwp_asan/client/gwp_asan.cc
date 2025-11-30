// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/gwp_asan/client/gwp_asan.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "base/allocator/partition_alloc_support.h"
#include "base/containers/flat_set.h"
#include "base/debug/crash_logging.h"
#include "base/feature_list.h"
#include "base/functional/callback_helpers.h"
#include "base/functional/function_ref.h"
#include "base/logging.h"
#include "base/metrics/field_trial_params.h"
#include "base/no_destructor.h"
#include "base/numerics/safe_math.h"
#include "base/rand_util.h"
#include "base/strings/strcat.h"
#include "build/build_config.h"
#include "components/crash/core/common/crash_key.h"
#include "components/gwp_asan/client/extreme_lightweight_detector_malloc_shims.h"
#include "components/gwp_asan/client/guarded_page_allocator.h"
#include "components/gwp_asan/client/gwp_asan_features.h"
#include "components/gwp_asan/client/lightweight_detector/poison_metadata_recorder.h"
#include "components/gwp_asan/client/sampling_helpers.h"
#include "components/gwp_asan/common/crash_key_name.h"
#include "partition_alloc/buildflags.h"

#if PA_BUILDFLAG(USE_PARTITION_ALLOC_AS_MALLOC) && PA_BUILDFLAG(IS_ANDROID)
#include "base/system/sys_info.h"
#endif  // PA_BUILDFLAG(USE_PARTITION_ALLOC_AS_MALLOC) &&
        // PA_BUILDFLAG(IS_ANDROID)

#if PA_BUILDFLAG(USE_ALLOCATOR_SHIM)
#include "components/gwp_asan/client/lightweight_detector/malloc_shims.h"
#include "components/gwp_asan/client/sampling_malloc_shims.h"
#endif  // PA_BUILDFLAG(USE_ALLOCATOR_SHIM)

#if PA_BUILDFLAG(USE_PARTITION_ALLOC)
#include "components/gwp_asan/client/lightweight_detector/partitionalloc_shims.h"
#include "components/gwp_asan/client/sampling_partitionalloc_shims.h"
#endif  // PA_BUILDFLAG(USE_PARTITION_ALLOC)

namespace gwp_asan {

namespace internal {
namespace {

[[maybe_unused]] constexpr bool kCpuIs64Bit =
#if defined(ARCH_CPU_64_BITS)
    true;
#else
    false;
#endif

// GWP-ASAN's default parameters are as follows:
// MaxAllocations determines the maximum number of simultaneous allocations
// allocated from the GWP-ASAN region.
//
// MaxMetadata determines the number of slots in the GWP-ASAN region that have
// associated metadata (e.g. alloc/dealloc stack traces).
//
// TotalPages determines the maximum number of slots used for allocations in the
// GWP-ASAN region. The defaults below use MaxMetadata * 2 on 32-bit builds
// (where OOMing due to lack of address space is a concern.)
//
// The allocation sampling frequency is calculated using the formula:
// SamplingMultiplier * AllocationSamplingRange**rand
// where rand is a random real number in the range [0,1).
//
// ProcessSamplingProbability is the probability of enabling GWP-ASAN in a new
// process.
//
// ProcessSamplingBoost is the multiplier to increase the
// ProcessSamplingProbability in scenarios where we want to perform additional
// testing (e.g., on canary/dev builds).
// 顶部默认参数定义处（平台默认值），调整 Windows 分支的容量
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS) || BUILDFLAG(IS_FUCHSIA)
constexpr int kDefaultMaxAllocations = 50;
constexpr int kDefaultMaxMetadata = 210;
constexpr int kDefaultTotalPages = kCpuIs64Bit ? 2048 : kDefaultMaxMetadata * 2;
constexpr int kDefaultAllocationSamplingMultiplier = 1500;
constexpr int kDefaultAllocationSamplingRange = 16;
constexpr double kDefaultProcessSamplingProbability = 0.01;
#elif BUILDFLAG(IS_ANDROID)
constexpr int kDefaultMaxAllocations = 70;
constexpr int kDefaultMaxMetadata = 255;
constexpr int kDefaultTotalPages = 512;
constexpr int kDefaultAllocationSamplingMultiplier = 2000;
constexpr int kDefaultAllocationSamplingRange = 20;
constexpr double kDefaultProcessSamplingProbability = 0.015;
#else
// Windows / 其他平台的默认值：调大容量，采样频率保持 1。
// 注意：AllocatorState::kMaxMetadata == 16384，因此 MaxMetadata=8192 安全。
// TotalPages=16384 不超过 kMaxRequestedSlots=32767。
// constexpr int kDefaultMaxAllocations = 4096;
// constexpr int kDefaultMaxMetadata = 8192;
// constexpr int kDefaultTotalPages = 16384;
// constexpr int kDefaultAllocationSamplingMultiplier = 1;
// constexpr int kDefaultAllocationSamplingRange = 1;
// constexpr double kDefaultProcessSamplingProbability = 0.015;

constexpr int kDefaultMaxAllocations = 70;
constexpr int kDefaultMaxMetadata = 255;
constexpr int kDefaultTotalPages = kCpuIs64Bit ? 2048 : kDefaultMaxMetadata * 2;
constexpr int kDefaultAllocationSamplingMultiplier = 1000;
constexpr int kDefaultAllocationSamplingRange = 16;
constexpr double kDefaultProcessSamplingProbability = 0.015;
#endif  // BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS) ||
        // BUILDFLAG(IS_FUCHSIA)
constexpr int kDefaultProcessSamplingBoost2 = 10;

#if defined(ARCH_CPU_64_BITS)
// The aim is to have the same memory overhead as the default GWP-ASan mode,
// which is:
//   sizeof(SlotMetadata) * kDefaultMaxMetadata +
//     sizeof(SystemPage) * kDefaultMaxAllocations
// The memory overhead of Lightweight UAF detector is:
//   sizeof(LightweightSlotMetadata) * kDefaultMaxLightweightMetadata
constexpr int kDefaultMaxLightweightMetadata = 3000;
#if PA_BUILDFLAG(USE_ALLOCATOR_SHIM)
constexpr int kDefaultMaxTotalSize = 65536;

// A set of parameters temporarily used by the random sampling LUD experiment.
constexpr int kDefaultTotalSizeHighWaterMark = kDefaultMaxTotalSize * 0.8;
constexpr int kDefaultTotalSizeLowWaterMark = kDefaultMaxTotalSize * 0.7;
constexpr int kDefaultEvictionChunkSize = 128;
constexpr int kDefaultEvictionTaskIntervalMs = 1000;

constexpr int kMaxMaxTotalSize = 2 * 1024 * 1024;
constexpr int kMaxEvictionChunkSize = 1024;
constexpr int kMaxEvictionTaskIntervalMs = 10000;
#endif  // PA_BUILDFLAG(USE_ALLOCATOR_SHIM)
#endif  // defined(ARCH_CPU_64_BITS)

BASE_FEATURE(kLightweightUafDetector,
             "LightweightUafDetector",
#if BUILDFLAG(IS_WIN)
             base::FEATURE_ENABLED_BY_DEFAULT
#else
             base::FEATURE_DISABLED_BY_DEFAULT
#endif
);

constexpr base::FeatureParam<LightweightDetectorMode>::Option
    kLightweightUafDetectorModeOptions[] = {
        {LightweightDetectorMode::kBrpQuarantine, "BrpQuarantine"},
        {LightweightDetectorMode::kRandom, "Random"}};

const base::FeatureParam<LightweightDetectorMode>
    kLightweightUafDetectorModeParam{&kLightweightUafDetector, "Mode",
                                     LightweightDetectorMode::kBrpQuarantine,
                                     &kLightweightUafDetectorModeOptions};

// Gets (integral) named `param` from `feature`,  defaulting to
// `fallback` if unset. Invokes `failure_condition()` on the result to
// validate that the value is acceptable.
std::optional<int> GetIntParam(const base::Feature& feature,
                               const std::string& param,
                               int fallback,
                               std::string_view process_type,
                               base::FunctionRef<bool(int)> failure_condition) {
  const std::optional<std::string_view> param_prefix =
      ProcessString(process_type);

  // Get the prefix-less parameter value first.
  int param_int = GetFieldTrialParamByFeatureAsInt(feature, param, fallback);
  LOG(INFO) << "[GWP-ASan][ParamRead] feature=" << feature.name
            << " process_type=" << process_type
            << " param=" << param
            << " raw_prefixless=\""
            << base::GetFieldTrialParamByFeatureAsString(feature, param, "")
            << "\""
            << " prefixless=" << param_int;
  if (param_prefix.has_value()) {
    // If a process-specific override parameter exists, prefer that
    // instead.
    const std::string prefixed_param =
        base::StrCat({param_prefix.value(), param});
    param_int =
        GetFieldTrialParamByFeatureAsInt(feature, prefixed_param, param_int);
        LOG(INFO) << "[GWP-ASan][ParamRead] feature=" << feature.name
                  << " process_type=" << std::string(process_type)
                  << " param=" << param
                  << " prefixed_key=" << prefixed_param
                  << " raw_prefixed=\""
                  << base::GetFieldTrialParamByFeatureAsString(feature, prefixed_param, "")
                  << "\""
                  << " prefixed=" << param_int;
  }

  if (param_int < 1 || failure_condition(param_int)) {
    DLOG(ERROR) << feature.name << " " << param
                << " is out-of-range: " << param_int;
    return std::nullopt;
  }
  return param_int;
}

// Returns whether this process should be sampled to enable GWP-ASan.
bool SampleProcess(const base::Feature& feature, bool boost_sampling) {
  double process_sampling_probability =
      GetFieldTrialParamByFeatureAsDouble(feature, "ProcessSamplingProbability",
                                          kDefaultProcessSamplingProbability);
  LOG(INFO) << "[GWP-ASan][SampleProcess] feature=" << feature.name
            << " boost_sampling=" << boost_sampling
            << " prob_param=" << process_sampling_probability
            << " raw_prob=\""
            << base::GetFieldTrialParamByFeatureAsString(
                   feature, "ProcessSamplingProbability", "")
            << "\"";
  if (process_sampling_probability < 0.0 ||
      process_sampling_probability > 1.0) {
    DLOG(ERROR) << feature.name
                << " ProcessSamplingProbability is out-of-range: "
                << process_sampling_probability;
    return false;
  }

  int process_sampling_boost = GetFieldTrialParamByFeatureAsInt(
      feature, "ProcessSamplingBoost2", kDefaultProcessSamplingBoost2);
  LOG(INFO) << "[GWP-ASan][SampleProcess] feature=" << feature.name
            << " boost_multiplier=" << process_sampling_boost
            << " raw_boost=\""
            << base::GetFieldTrialParamByFeatureAsString(
                   feature, "ProcessSamplingBoost2", "")
            << "\"";
  if (process_sampling_boost < 1) {
    DLOG(ERROR) << feature.name
                << " ProcessSampling multiplier is out-of-range: "
                << process_sampling_boost;
    return false;
  }

  base::CheckedNumeric<double> sampling_prob_mult =
      process_sampling_probability;
  if (boost_sampling)
    sampling_prob_mult *= process_sampling_boost;
  if (!sampling_prob_mult.IsValid()) {
    DLOG(ERROR) << feature.name << " multiplier caused out-of-range multiply: "
                << process_sampling_boost;
    return false;
  }

  process_sampling_probability = sampling_prob_mult.ValueOrDie();
  LOG(INFO) << "[GWP-ASan][SampleProcess] feature=" << feature.name
            << " final_probability=" << process_sampling_probability;
  return (base::RandDouble() < process_sampling_probability);
}

// Returns the allocation sampling frequency, or 0 on error.
size_t AllocationSamplingFrequency(const base::Feature& feature,
                                   std::string_view process_type) {
  std::optional<int> multiplier =
      GetIntParam(feature, "AllocationSamplingMultiplier",
                  kDefaultAllocationSamplingMultiplier, process_type,
                  [](int /*unused*/) { return false; });
  if (!multiplier.has_value()) {
    LOG(INFO) << "[GWP-ASan][AllocFreq] feature=" << feature.name
              << " process_type=" << process_type
              << " multiplier_missing";
    return 0;
  }

  std::optional<int> range = GetIntParam(
      feature, "AllocationSamplingRange", kDefaultAllocationSamplingRange,
      process_type, [](int _unused) { return false; });
  if (!range.has_value()) {
    LOG(INFO) << "[GWP-ASan][AllocFreq] feature=" << feature.name
              << " process_type=" << process_type
              << " range_missing";
    return 0;
  }
  LOG(INFO) << "[GWP-ASan][AllocFreq] feature=" << feature.name
            << " process_type=" << std::string(process_type)
            << " multiplier=" << multiplier.value()
            << " range=" << range.value();
  base::CheckedNumeric<size_t> frequency = multiplier.value();
  frequency *= std::pow(range.value(), base::RandDouble());
  if (!frequency.IsValid()) {
    DLOG(ERROR) << feature.name << "Out-of-range multiply "
                << multiplier.value() << " " << range.value();
    return 0;
  }

  LOG(INFO) << "[GWP-ASan][AllocFreq] feature=" << feature.name
            << " process_type=" << std::string(process_type)
            << " frequency=" << static_cast<unsigned long long>(frequency.ValueOrDie());
  return frequency.ValueOrDie();
}

// Don't use both GWP-ASan and LUD at the same time for performance
// reasons. When both features are enabled, we prefer GWP-ASan to
// compensate for its lower sampling rate.
bool IsMutuallyExclusiveFeatureAllowed(const base::Feature& feature) {
  static base::NoDestructor<base::flat_set<const base::Feature*>>
      disabled_features([]() {
        constexpr double kGwpAsanPickProbability = 0.9;

        base::flat_set<const base::Feature*> disabled_features;

        bool gwp_asan_enabled =
            base::FeatureList::IsEnabled(internal::kGwpAsanMalloc) ||
            base::FeatureList::IsEnabled(internal::kGwpAsanPartitionAlloc);
        bool lud_enabled =
            base::FeatureList::IsEnabled(internal::kLightweightUafDetector);
        if (gwp_asan_enabled && lud_enabled) {
          if (base::RandDouble() <= kGwpAsanPickProbability) {
            disabled_features.emplace(&internal::kLightweightUafDetector);
          } else {
            disabled_features.emplace(&internal::kGwpAsanMalloc);
            disabled_features.emplace(&internal::kGwpAsanPartitionAlloc);
          }
        }

        return disabled_features;
      }());

  return disabled_features->find(&feature) == disabled_features->end();
}

}  // namespace

// Exported for testing.
// Provides ungated access to the allocator settings that _would_
// be assigned to the `feature`.
GWP_ASAN_EXPORT std::optional<AllocatorSettings> GetAllocatorSettingsImpl(
    const base::Feature& feature,
    bool boost_sampling,
    std::string_view process_type) {
  static_assert(
      AllocatorState::kMaxRequestedSlots <= std::numeric_limits<int>::max(),
      "kMaxRequestedSlots out of range");
  constexpr int kMaxRequestedSlots =
      static_cast<int>(AllocatorState::kMaxRequestedSlots);

  static_assert(AllocatorState::kMaxMetadata <= std::numeric_limits<int>::max(),
                "AllocatorState::kMaxMetadata out of range");
  constexpr int kMaxMetadata = static_cast<int>(AllocatorState::kMaxMetadata);

  const auto total_pages =
      GetIntParam(feature, "TotalPages", kDefaultTotalPages, process_type,
                  [](int param_int) { return param_int > kMaxRequestedSlots; });
  if (!total_pages.has_value()) {
    LOG(INFO) << "[GWP-ASan][SettingsImpl] feature=" << feature.name
              << " process_type=" << process_type
              << " total_pages_missing";
    return std::nullopt;
  }
  LOG(INFO) << "[GWP-ASan][SettingsImpl] feature=" << feature.name
            << " process_type=" << process_type
            << " total_pages=" << total_pages.value();
  const auto max_metadata = GetIntParam(
      feature, "MaxMetadata", kDefaultMaxMetadata, process_type,
      [total_pages, kMaxMetadata](int param_int) {
        return param_int > std::min(total_pages.value(), kMaxMetadata);
      });
  if (!max_metadata.has_value()) {
    LOG(INFO) << "[GWP-ASan][SettingsImpl] feature=" << feature.name
              << " process_type=" << process_type
              << " max_metadata_missing";
    return std::nullopt;
  }
  LOG(INFO) << "[GWP-ASan][SettingsImpl] feature=" << feature.name
            << " process_type=" << process_type
            << " max_metadata=" << max_metadata.value();
  const auto max_allocations = GetIntParam(
      feature, "MaxAllocations", kDefaultMaxAllocations, process_type,
      [max_metadata](int param_int) { return param_int > max_metadata; });
  if (!max_allocations.has_value()) {
    LOG(INFO) << "[GWP-ASan][SettingsImpl] feature=" << feature.name
              << " process_type=" << process_type
              << " max_allocations_missing";
    return std::nullopt;
  }
  LOG(INFO) << "[GWP-ASan][SettingsImpl] feature=" << feature.name
            << " process_type=" << process_type
            << " max_allocations=" << max_allocations.value();
  const auto sampling_min_size =
      GetIntParam(feature, "SamplingMinSize", 1, process_type,
                  [](int /*unused*/) { return false; });
  if (!sampling_min_size.has_value()) {
    LOG(INFO) << "[GWP-ASan][SettingsImpl] feature=" << feature.name
              << " process_type=" << process_type
              << " sampling_min_size_missing";
    return std::nullopt;
  }
  LOG(INFO) << "[GWP-ASan][SettingsImpl] feature=" << feature.name
            << " process_type=" << process_type
            << " sampling_min_size=" << sampling_min_size.value();
  const auto sampling_max_size =
      GetIntParam(feature, "SamplingMaxSize", std::numeric_limits<int>::max(),
                  process_type, [sampling_min_size](int param_int) {
                    return param_int <= sampling_min_size;
                  });
  if (!sampling_max_size.has_value()) {
    LOG(INFO) << "[GWP-ASan][SettingsImpl] feature=" << feature.name
              << " process_type=" << process_type
              << " sampling_max_size_missing";
    return std::nullopt;
  }
  LOG(INFO) << "[GWP-ASan][SettingsImpl] feature=" << feature.name
            << " process_type=" << process_type
            << " sampling_max_size=" << sampling_max_size.value();
  size_t alloc_sampling_freq =
      AllocationSamplingFrequency(feature, process_type);
  if (!alloc_sampling_freq)
    LOG(INFO) << "[GWP-ASan][SettingsImpl] feature=" << feature.name
              << " process_type=" << process_type
              << " alloc_sampling_freq=0";
  if (!alloc_sampling_freq)
    return std::nullopt;
  LOG(INFO) << "[GWP-ASan][SettingsImpl] feature=" << feature.name
            << " process_type=" << process_type
            << " final_settings total_pages=" << total_pages.value()
            << " max_metadata=" << max_metadata.value()
            << " max_allocations=" << max_allocations.value()
            << " sampling_freq=" << alloc_sampling_freq
            << " min_size=" << sampling_min_size.value()
            << " max_size=" << sampling_max_size.value();
  return AllocatorSettings{static_cast<size_t>(max_allocations.value()),
                           static_cast<size_t>(max_metadata.value()),
                           static_cast<size_t>(total_pages.value()),
                           alloc_sampling_freq,
                           static_cast<size_t>(sampling_min_size.value()),
                           static_cast<size_t>(sampling_max_size.value())};
}

// Exported for testing.
GWP_ASAN_EXPORT std::optional<AllocatorSettings> GetAllocatorSettings(
    const base::Feature& feature,
    bool boost_sampling,
    std::string_view process_type) {
  LOG(INFO) << "[GWP-ASan][Settings] feature=" << feature.name
            << " boost_sampling=" << boost_sampling
            << " process_type=" << process_type;
  if (!base::FeatureList::IsEnabled(feature)) {
    LOG(INFO) << "[GWP-ASan][Settings] feature=" << feature.name
              << " disabled";
    return std::nullopt;
  }
  if (!IsMutuallyExclusiveFeatureAllowed(feature)) {
    LOG(INFO) << "[GWP-ASan][Settings] feature=" << feature.name
              << " rejected_by_mutual_exclusion";
    return std::nullopt;
  }
  if (!SampleProcess(feature, boost_sampling)) {
    LOG(INFO) << "[GWP-ASan][Settings] feature=" << feature.name
              << " process_not_sampled";
    return std::nullopt;
  }
  LOG(INFO) << "[GWP-ASan][Settings] feature=" << feature.name
            << " passed_all_checks";
  return GetAllocatorSettingsImpl(feature, boost_sampling, process_type);
}

bool MaybeEnableLightweightDetectorInternal(bool boost_sampling,
                                            const char* process_type) {
// The detector is not used on 32-bit systems because pointers there aren't big
// enough to safely store metadata IDs.
#if defined(ARCH_CPU_64_BITS)
  const auto& feature = kLightweightUafDetector;

  if (!base::FeatureList::IsEnabled(feature)) {
    return false;
  }

  if (!IsMutuallyExclusiveFeatureAllowed(feature)) {
    return false;
  }

  if (!SampleProcess(feature, boost_sampling)) {
    return false;
  }

  static_assert(
      LightweightDetectorState::kMaxMetadata <= std::numeric_limits<int>::max(),
      "LightweightDetectorState::kMaxMetadata out of range");
  constexpr int kMaxMetadata =
      static_cast<int>(LightweightDetectorState::kMaxMetadata);

  int max_metadata = GetFieldTrialParamByFeatureAsInt(
      feature, "MaxMetadata", kDefaultMaxLightweightMetadata);
  if (max_metadata < 1 || max_metadata > kMaxMetadata) {
    DLOG(ERROR) << feature.name
                << " MaxMetadata is out-of-range: " << max_metadata;
    return false;
  }

  switch (kLightweightUafDetectorModeParam.Get()) {
#if PA_BUILDFLAG(USE_PARTITION_ALLOC)
    case LightweightDetectorMode::kBrpQuarantine: {
      if (!base::allocator::PartitionAllocSupport::GetBrpConfiguration(
               process_type)
               .enable_brp) {
        return false;
      }

      lud::PoisonMetadataRecorder::Init(LightweightDetectorMode::kBrpQuarantine,
                                        static_cast<size_t>(max_metadata));
      static crash_reporter::CrashKeyString<24> crash_key(
          kLightweightDetectorCrashKey);
      crash_key.Set(lud::PoisonMetadataRecorder::Get()->GetCrashKey());
      lud::InstallPartitionAllocHooks();
      return true;
    }
#endif  // PA_BUILDFLAG(USE_PARTITION_ALLOC)

#if PA_BUILDFLAG(USE_ALLOCATOR_SHIM)
    case LightweightDetectorMode::kRandom: {
      int max_allocations = GetFieldTrialParamByFeatureAsInt(
          feature, "MaxAllocations", kDefaultMaxAllocations);
      if (max_allocations < 1 || max_allocations > max_metadata) {
        DLOG(ERROR) << feature.name
                    << " MaxAllocations is out-of-range: " << max_allocations
                    << " with MaxMetadata = " << max_metadata;
        return false;
      }

      int max_total_size = GetFieldTrialParamByFeatureAsInt(
          feature, "MaxTotalSize", kDefaultMaxTotalSize);
      if (max_total_size < 1 || max_total_size > kMaxMaxTotalSize) {
        DLOG(ERROR) << feature.name
                    << " MaxTotalSize is out-of-range: " << max_total_size;
        return false;
      }

      int total_size_high_water_mark = GetFieldTrialParamByFeatureAsInt(
          feature, "TotalSizeHighWaterMark", kDefaultTotalSizeHighWaterMark);
      if (total_size_high_water_mark < 1 ||
          total_size_high_water_mark >= max_total_size) {
        DLOG(ERROR) << feature.name
                    << " TotalSizeHighWaterMark is out-of-range: "
                    << total_size_high_water_mark;
        return false;
      }

      int total_size_low_water_mark = GetFieldTrialParamByFeatureAsInt(
          feature, "TotalSizeLowWaterMark", kDefaultTotalSizeLowWaterMark);
      if (total_size_low_water_mark < 1 ||
          total_size_low_water_mark >= total_size_high_water_mark) {
        DLOG(ERROR) << feature.name
                    << " TotalSizeLowWaterMark is out-of-range: "
                    << total_size_low_water_mark;
        return false;
      }

      int eviction_chunk_size = GetFieldTrialParamByFeatureAsInt(
          feature, "EvictionChunkSize", kDefaultEvictionChunkSize);
      if (eviction_chunk_size < 1 ||
          eviction_chunk_size > kMaxEvictionChunkSize) {
        DLOG(ERROR) << feature.name << " EvictionChunkSize is out-of-range: "
                    << eviction_chunk_size;
        return false;
      }

      int eviction_task_interval_ms = GetFieldTrialParamByFeatureAsInt(
          feature, "EvictionTaskIntervalMs", kDefaultEvictionTaskIntervalMs);
      if (eviction_task_interval_ms < 1 ||
          eviction_task_interval_ms > kMaxEvictionTaskIntervalMs) {
        DLOG(ERROR) << feature.name
                    << " EvictionTaskIntervalMs is out-of-range: "
                    << eviction_task_interval_ms;
        return false;
      }

      // LUD (currently) does not vary its sampling frequency by process
      // type, so we should avoid passing a valid process type to
      // `AllocationSamplingFrequency()` (to force it not to fetch
      // process-specific parameters).
      constexpr std::string_view kDummyProcessType = "invalid process type";
      size_t alloc_sampling_freq =
          AllocationSamplingFrequency(feature, kDummyProcessType);
      if (!alloc_sampling_freq) {
        return false;
      }

      lud::PoisonMetadataRecorder::Init(LightweightDetectorMode::kRandom,
                                        static_cast<size_t>(max_metadata));
      static crash_reporter::CrashKeyString<24> crash_key(
          kLightweightDetectorCrashKey);
      crash_key.Set(lud::PoisonMetadataRecorder::Get()->GetCrashKey());
      lud::InstallMallocHooks(static_cast<size_t>(max_allocations),
                              static_cast<size_t>(max_total_size),
                              static_cast<size_t>(total_size_high_water_mark),
                              static_cast<size_t>(total_size_low_water_mark),
                              static_cast<size_t>(eviction_chunk_size),
                              static_cast<size_t>(eviction_task_interval_ms),
                              alloc_sampling_freq);
      return true;
    }
#endif  // PA_BUILDFLAG(USE_ALLOCATOR_SHIM)

    default: {
      DLOG(ERROR) << "Unsupported Lightweight UAF Detector mode.";
      return false;
    }
  }
#else   // defined(ARCH_CPU_64_BITS)
  std::ignore = boost_sampling;
  std::ignore = process_type;
  std::ignore = kLightweightUafDetectorModeParam;
  return false;
#endif  // defined(ARCH_CPU_64_BITS)
}

}  // namespace internal

void EnableForMalloc(bool boost_sampling, std::string_view process_type) {
  LOG(INFO) << "GWP-ASan(Malloc) enter boost_sampling=" << boost_sampling
            << " process_type=" << std::string(process_type);
#if PA_BUILDFLAG(USE_ALLOCATOR_SHIM)
  static bool init_once = [&]() -> bool {
    LOG(INFO) << "GWP-ASan(Malloc) boost_sampling=" << boost_sampling
              << " process_type=" << std::string(process_type);
    const auto settings = internal::GetAllocatorSettings(
        internal::kGwpAsanMalloc, boost_sampling, process_type);
    bool activated_gwp_asan = false;
    if (settings.has_value()) {
      activated_gwp_asan = internal::InstallMallocHooks(
          settings.value(),
          internal::CreateOomCallback("Malloc", process_type,
                                      settings->sampling_frequency));
    }
    internal::ReportGwpAsanActivated("Malloc", process_type,
                                     activated_gwp_asan);

    return activated_gwp_asan;
  }();
  std::ignore = init_once;
#else
  std::ignore = internal::kGwpAsanMalloc;
  DLOG(WARNING) << "base::allocator shims are unavailable for GWP-ASan.";
#endif  // PA_BUILDFLAG(USE_ALLOCATOR_SHIM)
}

void EnableForPartitionAlloc(bool boost_sampling,
                             std::string_view process_type) {
#if PA_BUILDFLAG(USE_PARTITION_ALLOC)
  static bool init_once = [&]() -> bool {
    LOG(INFO) << "GWP-ASan(PartitionAlloc) boost_sampling=" << boost_sampling
              << " process_type=" << process_type;
    const auto settings = internal::GetAllocatorSettings(
        internal::kGwpAsanPartitionAlloc, boost_sampling, process_type);
    bool activated_gwp_asan = false;
    if (settings.has_value()) {
      activated_gwp_asan = internal::InstallPartitionAllocHooks(
          settings.value(),
          internal::CreateOomCallback("PartitionAlloc", process_type,
                                      settings->sampling_frequency));
    }
    internal::ReportGwpAsanActivated("PartitionAlloc", process_type,
                                     activated_gwp_asan);

    // 增加激活确认日志
    LOG(INFO) << "GWP-ASan(PartitionAlloc) activated="
              << activated_gwp_asan;

    LOG(INFO) << "GWP-ASan(PartitionAlloc) settings_has_value="
              << settings.has_value();
    if (settings.has_value()) {
      LOG(INFO) << "GWP-ASan(PartitionAlloc) params: total_pages="
                << settings->total_pages
                << " max_metadata=" << settings->num_metadata
                << " max_allocated_pages=" << settings->max_allocated_pages
                << " sampling_freq=" << settings->sampling_frequency
                << " min_size=" << settings->sampling_min_size
                << " max_size=" << settings->sampling_max_size;
    }
    return activated_gwp_asan;
  }();
  std::ignore = init_once;
#else
  std::ignore = internal::kGwpAsanPartitionAlloc;
  DLOG(WARNING) << "PartitionAlloc hooks are unavailable for GWP-ASan.";
#endif  // PA_BUILDFLAG(USE_PARTITION_ALLOC)
}

void MaybeEnableLightweightDetector(bool boost_sampling,
                                    const char* process_type) {
  [[maybe_unused]] static bool init_once =
      internal::MaybeEnableLightweightDetectorInternal(boost_sampling,
                                                       process_type);
}

void MaybeEnableExtremeLightweightDetector(bool boost_sampling,
                                           std::string_view process_type) {
#if PA_BUILDFLAG(USE_PARTITION_ALLOC_AS_MALLOC)
#if PA_BUILDFLAG(IS_ANDROID)
  // The negative performance impacts of ELUD are not negligible, thus we'd like
  // to apply ELUD only to high memory devices (approximately high-end devices)
  // in case of Android. On other platforms, the performance impacts are
  // acceptable.
  //
  // It is very important to filter this condition before
  // `base::FeatureList::IsEnabled` gets called so that the finch system applies
  // the experiments to the right devices equally and collects the accurate
  // statistics from the devices.
  if (base::SysInfo::AmountOfPhysicalMemory() < base::GiB(8)) {
    return;
  }
#endif  // PA_BUILDFLAG(IS_ANDROID)

  if (!base::FeatureList::IsEnabled(internal::kExtremeLightweightUAFDetector)) {
    return;
  }

  using enum internal::ExtremeLightweightUAFDetectorTargetProcesses;
  switch (internal::kExtremeLightweightUAFDetectorTargetProcesses.Get()) {
    case kAllProcesses:
      break;
    case kBrowserProcessOnly:
      if (!process_type.empty()) {
        return;  // Non-empty process_type means a non-browser process.
      }
      break;
    case kNonRendererProcesses:
      if (process_type == "renderer") {
        return;
      }
      break;
  }

  [[maybe_unused]] static bool init_once = [&]() -> bool {
    size_t sampling_frequency = static_cast<size_t>(
        internal::kExtremeLightweightUAFDetectorSamplingFrequency.Get());
    size_t quarantine_capacity_for_small_objects_in_bytes = static_cast<size_t>(
        internal::
            kExtremeLightweightUAFDetectorQuarantineCapacityForSmallObjectsInBytes
                .Get());
    size_t quarantine_capacity_for_large_objects_in_bytes = static_cast<size_t>(
        internal::
            kExtremeLightweightUAFDetectorQuarantineCapacityForLargeObjectsInBytes
                .Get());
    size_t object_size_threshold_in_bytes = static_cast<size_t>(
        internal::kExtremeLightweightUAFDetectorObjectSizeThresholdInBytes
            .Get());
    internal::InstallExtremeLightweightDetectorHooks(
        {.sampling_frequency = sampling_frequency,
         .quarantine_capacity_for_small_objects_in_bytes =
             quarantine_capacity_for_small_objects_in_bytes,
         .quarantine_capacity_for_large_objects_in_bytes =
             quarantine_capacity_for_large_objects_in_bytes,
         .object_size_threshold_in_bytes = object_size_threshold_in_bytes});
    return true;
  }();
#endif  // PA_BUILDFLAG(USE_PARTITION_ALLOC_AS_MALLOC)
}

}  // namespace gwp_asan
