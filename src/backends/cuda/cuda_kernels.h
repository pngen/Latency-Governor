#pragma once

// C ABI boundary between the MSVC-compiled host wrapper (cuda_backend.cpp) and
// the nvcc-compiled device source (cuda_kernels.cu). Keeping CUDA headers out of
// the host wrapper avoids MSVC/CUDA host-compiler friction.
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns 0 on success. Fills out_checksum (deterministic fingerprint) and
// out_duration_us (wall-clock nanoseconds of the device work measured by the
// host around the whole alloc/copy/launch/sync/copy/free sequence).
int lg_cuda_available(int device, const char** info);
int lg_cuda_memory(int device, unsigned long long* total, unsigned long long* free_mem);

int lg_cuda_run_prefill(const float* h_in, size_t n, size_t reps,
                        float* h_out, unsigned long long* out_checksum);
int lg_cuda_run_decode(const float* h_in, size_t n, size_t steps,
                       float* h_out, unsigned long long* out_checksum);

#ifdef __cplusplus
} // extern "C"
#endif
