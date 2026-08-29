# CPU backend (always available).
add_library(latency_governor_backends STATIC cpu/cpu_backend.cpp)
add_library(latency_governor::backends ALIAS latency_governor_backends)
target_link_libraries(latency_governor_backends PUBLIC latency_governor)
target_include_directories(latency_governor_backends PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
lg_apply_warnings(latency_governor_backends)

# CUDA backend (real accelerator execution; optional).
if(LATENCY_GOVERNOR_ENABLE_CUDA)
    find_package(CUDAToolkit REQUIRED)
    add_library(latency_governor_backends_cuda STATIC
        cuda/cuda_backend.cpp
        cuda/cuda_kernels.cu)
    add_library(latency_governor::backends_cuda ALIAS latency_governor_backends_cuda)
    target_link_libraries(latency_governor_backends_cuda PUBLIC latency_governor CUDA::cudart)
    target_include_directories(latency_governor_backends_cuda PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/cuda")
    set_target_properties(latency_governor_backends_cuda PROPERTIES
        CUDA_ARCHITECTURES 120
        POSITION_INDEPENDENT_CODE ON)
    # /W4 /WX for the host wrapper only. nvcc must not receive MSVC warning flags.
    set_source_files_properties(cuda/cuda_backend.cpp PROPERTIES COMPILE_OPTIONS "/W4;/WX;/permissive-;/Zc:__cplusplus;/EHsc")
endif()
