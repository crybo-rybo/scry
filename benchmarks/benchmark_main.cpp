#include <benchmark/benchmark.h>
#include <cstdlib>
#include <iostream>
#include <string>

#if defined(SCRY_VALIDATE_ALLOCATION_TRACKER)
#include "allocation_tracker.hpp"
#endif

#ifndef SCRY_BENCHMARK_MODE
#error "SCRY_BENCHMARK_MODE must describe this benchmark executable"
#endif

#ifndef SCRY_BENCHMARK_TARGET
#error "SCRY_BENCHMARK_TARGET must identify this benchmark executable"
#endif

int main(int argc, char** argv) {
#if defined(SCRY_VALIDATE_ALLOCATION_TRACKER)
  if (!scry::bench::validate_allocation_tracker()) {
    std::cerr << "C++ allocation tracker self-check failed\n";
    return 1;
  }
#endif
  const auto* run_id = std::getenv("SCRY_BENCHMARK_RUN_ID");
  if (run_id == nullptr || std::string{run_id}.empty()) {
    std::cerr << "SCRY_BENCHMARK_RUN_ID must bind output to a profiling run\n";
    return 1;
  }
  benchmark::AddCustomContext("scry_benchmark_schema", "1");
  benchmark::AddCustomContext("scry_fixture_seed", "0");
  benchmark::AddCustomContext("scry_measurement_mode", SCRY_BENCHMARK_MODE);
  benchmark::AddCustomContext("scry_benchmark_target", SCRY_BENCHMARK_TARGET);
  benchmark::AddCustomContext("scry_run_id", run_id);
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
