#include "cuda_kernels.h"

#include <cstdio>
#include <cuda_runtime.h>

// Prefill-like bulk kernel: multiply-accumulate over an array (independent
// elements), repeated to create a realistic prefill cost.
__global__ void lg_prefill_kernel(float* out, const float* in, int n, int reps) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        float acc = in[i];
        for (int k = 0; k < reps; ++k) acc = fmaf(acc, 1.00001f, 0.5f);
        out[i] = acc;
    }
}

// Decode-like iterative kernel: sequential state mutation over steps (each step
// depends on the prior), simulating iterative token generation whose cost is
// per-step and latency-sensitive.
__global__ void lg_decode_kernel(float* state, const float* weights, int n, int steps) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        float s = state[i];
        float w = weights[i];
        for (int k = 0; k < steps; ++k) s = s * 1.000001f + w * 0.0001f;
        state[i] = s;
    }
}

static int cur_device(int device) {
    if (cudaSetDevice(device) != cudaSuccess) { /* ignore, use current */ }
    int cur = 0;
    cudaGetDevice(&cur);
    return cur;
}

int lg_cuda_available(int device, const char** info) {
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess) return -1;
    if (count <= 0) return -1;
    cudaDeviceProp prop;
    int cur = cur_device(device);
    if (cudaGetDeviceProperties(&prop, cur) != cudaSuccess) return -1;
    static char buf[256];
    std::snprintf(buf, sizeof(buf), "cuda:device=%d name=%s cc=%d.%d mem=%zuMiB",
                  cur, prop.name, prop.major, prop.minor, (size_t)(prop.totalGlobalMem / (1024 * 1024)));
    if (info) *info = buf;
    return 0;
}

int lg_cuda_memory(int device, unsigned long long* total, unsigned long long* free_mem) {
    int cur = cur_device(device);
    size_t t = 0, f = 0;
    if (cudaMemGetInfo(&f, &t) != cudaSuccess) return -1;
    if (total) *total = (unsigned long long)t;
    if (free_mem) *free_mem = (unsigned long long)f;
    return 0;
}

int lg_cuda_run_prefill(const float* h_in, size_t n, size_t reps,
                        float* h_out, unsigned long long* out_checksum) {
    size_t bytes = n * sizeof(float);
    float *d_in = nullptr, *d_out = nullptr;
    int rc = 0;
    int nb = (int)n;
    int block = 256, grid = (nb + block - 1) / block;
    if (cudaMalloc(&d_in, bytes) != cudaSuccess) return -1;
    if (cudaMalloc(&d_out, bytes) != cudaSuccess) { cudaFree(d_in); return -1; }
    if (cudaMemcpy(d_in, h_in, bytes, cudaMemcpyHostToDevice) != cudaSuccess) { rc = -1; goto done; }
    lg_prefill_kernel<<<grid, block>>>(d_out, d_in, nb, (int)reps);
    if (cudaDeviceSynchronize() != cudaSuccess) { rc = -1; goto done; }
    if (cudaMemcpy(h_out, d_out, bytes, cudaMemcpyDeviceToHost) != cudaSuccess) { rc = -1; goto done; }
    {   // deterministic fingerprints: sum and xor of the buffer
        unsigned long long sum = 0, xr = 0;
        for (size_t i = 0; i < n; ++i) {
            unsigned long long v = (unsigned long long)(unsigned int)(h_out[i] * 1000.0f);
            sum += v; xr ^= v * 0x9E3779B97F4A7C15ULL;
        }
        *out_checksum = sum ^ xr;
    }
done:
    cudaGetLastError();
    cudaFree(d_out); cudaFree(d_in);
    return rc;
}

int lg_cuda_run_decode(const float* h_in, size_t n, size_t steps,
                       float* h_out, unsigned long long* out_checksum) {
    size_t bytes = n * sizeof(float);
    float *d_state = nullptr, *d_w = nullptr;
    int rc = 0;
    int nb = (int)n;
    int block = 256, grid = (nb + block - 1) / block;
    if (cudaMalloc(&d_state, bytes) != cudaSuccess) return -1;
    if (cudaMalloc(&d_w, bytes) != cudaSuccess) { cudaFree(d_state); return -1; }
    if (cudaMemcpy(d_state, h_in, bytes, cudaMemcpyHostToDevice) != cudaSuccess) { rc = -1; goto done; }
    // weights derived from the same input for determinism (copy into a separate buffer).
    if (cudaMemcpy(d_w, h_in, bytes, cudaMemcpyHostToDevice) != cudaSuccess) { rc = -1; goto done; }
    lg_decode_kernel<<<grid, block>>>(d_state, d_w, nb, (int)steps);
    if (cudaDeviceSynchronize() != cudaSuccess) { rc = -1; goto done; }
    if (cudaMemcpy(h_out, d_state, bytes, cudaMemcpyDeviceToHost) != cudaSuccess) { rc = -1; goto done; }
    {   unsigned long long sum = 0, xr = 0;
        for (size_t i = 0; i < n; ++i) {
            unsigned long long v = (unsigned long long)(unsigned int)(h_out[i] * 1000.0f);
            sum += v; xr ^= v * 0x9E3779B97F4A7C15ULL;
        }
        *out_checksum = sum ^ xr;
    }
done:
    cudaGetLastError();
    cudaFree(d_state); cudaFree(d_w);
    return rc;
}