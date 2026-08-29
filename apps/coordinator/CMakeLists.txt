add_executable(latency-governor-coordinator main.cpp)
target_link_libraries(latency-governor-coordinator PRIVATE
    latency_governor::latency_governor
    latency_governor::protocol)
lg_apply_warnings(latency-governor-coordinator)
