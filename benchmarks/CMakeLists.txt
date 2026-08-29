# benchmarks/CMakeLists.txt
# Benchmarks measure COMPLETED work: each loop finishes all its operations
# before the timing ends, and each reports ops/s + throughput.
#
# Core benchmarks link latency_governor only; the CUDA benchmark also links the
# real CUDA backend.

function(lg_add_bench name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ${ARGN})
    lg_apply_warnings(${name})
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks")
endfunction()

lg_add_bench(bench_core bench_core.cpp latency_governor)
lg_add_bench(bench_scale bench_scale.cpp latency_governor)
lg_add_bench(bench_threads bench_threads.cpp latency_governor)
lg_add_bench(bench_protocol bench_protocol.cpp latency_governor)
lg_add_bench(bench_cuda bench_cuda.cpp latency_governor latency_governor_backends latency_governor_backends_cuda)
