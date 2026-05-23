find_package(benchmark QUIET REQUIRED)

if (benchmark_FOUND)
  set_target_properties(benchmark::benchmark
                        PROPERTIES IMPORTED_GLOBAL TRUE)
else ()
  message(STATUS "Try to set benchmark_DIR, for example cmake -Dbenchmark_DIR=/usr/local")
endif ()
