cmake_minimum_required(VERSION 3.25)

add_executable(bench_harness bench_main.cpp)
target_compile_features(bench_harness PRIVATE cxx_std_20)
target_include_directories(bench_harness PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(MSVC)
  target_compile_options(bench_harness PRIVATE /W4 /WX /O2)
else()
  target_compile_options(bench_harness PRIVATE -Wall -Wextra -Werror -O3)
endif()

include(CTest)
add_test(NAME PerfBenchmark
  COMMAND bench_harness --output ${CMAKE_BINARY_DIR}/PERF_REPORT.md
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(PerfBenchmark PROPERTIES TIMEOUT 120)
install(TARGETS bench_harness DESTINATION dist)
