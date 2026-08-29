add_executable(latency-governorctl main.cpp)
target_link_libraries(latency-governorctl PRIVATE
    latency_governor::latency_governor
    latency_governor::protocol
    latency_governor::backends)
lg_apply_warnings(latency-governorctl)
