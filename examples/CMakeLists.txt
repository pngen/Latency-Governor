# examples/CMakeLists.txt
# Runnable examples for the Latency Governor public API.
# Each is a single .cpp; examples that use only the core link latency_governor,
# ones that use a backend also link latency_governor_backends (and the CUDA
# backend for the CUDA example).

function(lg_add_example name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ${ARGN})
    lg_apply_warnings(${name})
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples")
endfunction()

lg_add_example(example_01_contract 01_contract.cpp latency_governor)
lg_add_example(example_02_realtime_vs_throughput 02_realtime_vs_throughput.cpp latency_governor)
lg_add_example(example_03_queue_budget 03_queue_budget.cpp latency_governor)
lg_add_example(example_04_batch_seal 04_batch_seal.cpp latency_governor)
lg_add_example(example_05_ttft_protection 05_ttft_protection.cpp latency_governor)
lg_add_example(example_06_decode_inter_token 06_decode_inter_token.cpp latency_governor)
lg_add_example(example_07_speculation 07_speculation.cpp latency_governor)
lg_add_example(example_08_transfer_vs_local 08_transfer_vs_local.cpp latency_governor)
lg_add_example(example_09_retry_denial 09_retry_denial.cpp latency_governor)
lg_add_example(example_10_fairness 10_fairness.cpp latency_governor)
lg_add_example(example_11_policy_hot_replace 11_policy_hot_replace.cpp latency_governor)
lg_add_example(example_12_distributed 12_distributed.cpp latency_governor)
lg_add_example(example_13_persistence 13_persistence.cpp latency_governor)
lg_add_example(example_14_cuda 14_cuda_governed.cpp latency_governor latency_governor_backends latency_governor_backends_cuda)
