add_executable(latency-governor-worker main.cpp)
target_link_libraries(latency-governor-worker PRIVATE
    latency_governor::latency_governor
    latency_governor::protocol
    latency_governor::backends)
if(LATENCY_GOVERNOR_ENABLE_CUDA)
    target_link_libraries(latency-governor-worker PRIVATE latency_governor::backends_cuda)
    target_compile_definitions(latency-governor-worker PRIVATE LG_HAS_CUDA=1)
endif()
lg_apply_warnings(latency-governor-worker)
