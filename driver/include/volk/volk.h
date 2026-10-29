// Minimal volk stand-in: dsp/stream.h + buffer.h only need these three.
// SDR++'s volk.dll exports them C++-mangled (?volk_malloc@@YAPEAX_K0@Z), so no extern "C" here.
#pragma once
#include <stddef.h>
__declspec(dllimport) size_t volk_get_alignment(void);
__declspec(dllimport) void* volk_malloc(size_t size, size_t alignment);
__declspec(dllimport) void volk_free(void* aptr);
