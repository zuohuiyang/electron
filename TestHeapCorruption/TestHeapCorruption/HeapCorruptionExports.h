#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifdef TESTHEAPCORRUPTION_EXPORTS
#define THC_API __declspec(dllexport)
#else
#define THC_API __declspec(dllimport)
#endif

THC_API void TriggerHeapOverflow();
THC_API void TriggerUAF();
THC_API void TriggerDoubleFree();

#ifdef __cplusplus
}
#endif