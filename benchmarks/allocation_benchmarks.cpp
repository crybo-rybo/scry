#include "allocation_tracker.hpp"
#include "scenarios.hpp"

#include <benchmark/benchmark.h>
#include <cstdint>
#include <utility>

namespace scry::bench {
namespace {

void publish_result(benchmark::State& state, const ScenarioResult& result,
                    const AllocationSample totals) {
  const auto checksum_hi = static_cast<std::uint32_t>(result.digest >> 32U);
  const auto checksum_lo = static_cast<std::uint32_t>(result.digest & 0xffff'ffffULL);
  state.counters["checksum_hi"] = static_cast<double>(checksum_hi);
  state.counters["checksum_lo"] = static_cast<double>(checksum_lo);
  state.counters["cpp_allocations"] = benchmark::Counter(
      static_cast<double>(totals.calls), benchmark::Counter::kAvgIterations);
  state.counters["cpp_requested_bytes"] = benchmark::Counter(
      static_cast<double>(totals.requested_bytes), benchmark::Counter::kAvgIterations);
  state.counters["cpp_live_requested_bytes"] =
      benchmark::Counter(static_cast<double>(totals.live_requested_bytes),
                         benchmark::Counter::kAvgIterations);
  state.counters["cpp_peak_live_requested_bytes"] =
      benchmark::Counter(static_cast<double>(totals.peak_live_requested_bytes),
                         benchmark::Counter::kAvgIterations);
  state.counters["input_bytes"] = static_cast<double>(result.input_bytes);
  state.counters["items"] = static_cast<double>(result.items);
  state.counters["output_bytes"] = static_cast<double>(result.output_bytes);
}

template <typename Factory>
void measure_allocations(benchmark::State& state, Factory&& factory) {
  auto scenario = std::forward<Factory>(factory)();
  auto last = scenario.run(true);
  benchmark::DoNotOptimize(last.digest);
  if (!last.valid) {
    state.SkipWithError("scenario warm-up validation failed");
    publish_result(state, last, {});
    return;
  }
  AllocationSample totals{};
  for (auto iteration : state) {
    static_cast<void>(iteration);
    AllocationScope tracking{};
    if (!tracking.valid()) {
      state.SkipWithError("overlapping allocation measurement rejected");
      break;
    }
    last = scenario.run(false);
    const auto sample = tracking.finish();
    if (!sample.valid) {
      state.SkipWithError("allocation measurement epoch invalidated");
      break;
    }
    totals.calls += sample.calls;
    totals.requested_bytes += sample.requested_bytes;
    totals.live_requested_bytes += sample.live_requested_bytes;
    totals.peak_live_requested_bytes += sample.peak_live_requested_bytes;
    benchmark::DoNotOptimize(last.items);
    benchmark::DoNotOptimize(last.output_bytes);
    if (!last.valid) {
      state.SkipWithError("scenario semantic validation failed");
      break;
    }
  }
  publish_result(state, last, totals);
}

template <typename Factory>
void measure_prepared_allocations(benchmark::State& state, Factory&& factory) {
  auto scenario = std::forward<Factory>(factory)();
  auto last = scenario.validate();
  benchmark::DoNotOptimize(last.digest);
  if (!last.valid) {
    state.SkipWithError("scenario warm-up validation failed");
    publish_result(state, last, {});
    return;
  }
  AllocationSample totals{};
  for (auto iteration : state) {
    static_cast<void>(iteration);
    auto operation = scenario.prepare();
    AllocationScope tracking{};
    if (!tracking.valid()) {
      state.SkipWithError("overlapping allocation measurement rejected");
      break;
    }
    last = operation.run();
    const auto sample = tracking.finish();
    if (!sample.valid) {
      state.SkipWithError("allocation measurement epoch invalidated");
      break;
    }
    totals.calls += sample.calls;
    totals.requested_bytes += sample.requested_bytes;
    totals.live_requested_bytes += sample.live_requested_bytes;
    totals.peak_live_requested_bytes += sample.peak_live_requested_bytes;
    benchmark::DoNotOptimize(last.items);
    benchmark::DoNotOptimize(last.output_bytes);
    if (!last.valid) {
      state.SkipWithError("scenario semantic validation failed");
      break;
    }
  }
  publish_result(state, last, totals);
}

void SSE(benchmark::State& state) {
  const auto chunk_bytes = static_cast<std::size_t>(state.range(0));
  const auto use_crlf = state.range(1) != 0;
  measure_allocations(state, [=] { return SseScenario{chunk_bytes, use_crlf}; });
}

void OpenAIStream(benchmark::State& state) {
  const auto shape =
      state.range(0) == 0 ? OpenAiStreamShape::text : OpenAiStreamShape::tools;
  const auto scale = static_cast<std::size_t>(state.range(1));
  measure_allocations(state, [=] { return OpenAiStreamScenario{shape, scale}; });
}

void RequestEncoding(benchmark::State& state) {
  const auto dialect =
      state.range(0) == 0 ? RequestDialect::openai : RequestDialect::anthropic;
  const auto message_count = static_cast<std::size_t>(state.range(1));
  const auto schema_count = static_cast<std::size_t>(state.range(2));
  measure_allocations(state, [=] {
    return RequestEncodingScenario{dialect, message_count, schema_count};
  });
}

void TurnMachine(benchmark::State& state) {
  const auto tool_count = static_cast<std::size_t>(state.range(0));
  const auto argument_bytes = static_cast<std::size_t>(state.range(1));
  measure_prepared_allocations(
      state, [=] { return TurnMachineScenario{tool_count, argument_bytes}; });
}

void Pump(benchmark::State& state) {
  const auto shape =
      state.range(0) == 0 ? PumpShape::text_delivery : PumpShape::completion_commit;
  const auto route_count = static_cast<std::size_t>(state.range(1));
  measure_prepared_allocations(state, [=] { return PumpScenario{shape, route_count}; });
}

void Admission(benchmark::State& state, const AdmissionShape shape) {
  const auto element_count = static_cast<std::size_t>(state.range(0));
  const auto element_bytes = static_cast<std::size_t>(state.range(1));
  measure_prepared_allocations(
      state, [=] { return AdmissionScenario{shape, element_count, element_bytes}; });
}

void ToolRegistry(benchmark::State& state, const bool /*registration*/) {
  const auto schema_count = static_cast<std::size_t>(state.range(0));
  measure_prepared_allocations(state,
                               [=] { return ToolRegistryScenario{schema_count}; });
}

void SchemaAdmission(benchmark::State& state, const SchemaAdmissionShape shape) {
  const auto schema_count = static_cast<std::size_t>(state.range(0));
  const auto schema_bytes = static_cast<std::size_t>(state.range(1));
  measure_prepared_allocations(state, [=] {
    return SchemaAdmissionScenario{shape, schema_count, schema_bytes};
  });
}

void HistoryCommit(benchmark::State& state, const HistoryCommitShape shape) {
  const auto message_count = static_cast<std::size_t>(state.range(0));
  const auto message_bytes = static_cast<std::size_t>(state.range(1));
  measure_prepared_allocations(state, [=] {
    return HistoryCommitScenario{shape, message_count, message_bytes};
  });
}

BENCHMARK(SSE)
    ->Args({1, 0})
    ->Args({64, 0})
    ->Args({4096, 0})
    ->Args({0, 0})
    ->Args({1, 1})
    ->Args({64, 1})
    ->Args({4096, 1})
    ->Args({0, 1})
    ->ArgNames({"chunk_bytes", "crlf"});

BENCHMARK(OpenAIStream)
    ->Args({0, 128})
    ->Args({1, 1})
    ->Args({1, 8})
    ->Args({1, 32})
    ->ArgNames({"shape", "scale"});

BENCHMARK(RequestEncoding)
    ->ArgsProduct({{0, 1}, {0, 32, 256}, {0, 16, 64}})
    ->ArgNames({"dialect", "messages", "schemas"});

BENCHMARK(TurnMachine)
    ->ArgsProduct({{1, 8, 32}, {1024, 16 * 1024}})
    ->ArgNames({"tools", "argument_bytes"});

BENCHMARK(Pump)
    ->Args({0, 64})
    ->Args({1, 64})
    ->ArgNames({"shape", "routes"})
    ->UseRealTime();

BENCHMARK_CAPTURE(Admission, History, AdmissionShape::history)
    ->Args({32, 4096})
    ->Args({256, 512})
    ->Args({2048, 64})
    ->ArgNames({"messages", "message_bytes"})
    ->Iterations(1)
    ->UseRealTime();

BENCHMARK_CAPTURE(Admission, Schemas, AdmissionShape::schemas)
    ->Args({8, 2048})
    ->Args({32, 2048})
    ->Args({64, 2048})
    ->ArgNames({"schemas", "schema_bytes"})
    ->Iterations(1)
    ->UseRealTime();

BENCHMARK_CAPTURE(SchemaAdmission, ColdAccepted, SchemaAdmissionShape::cold_accepted)
    ->Args({8, 2048})
    ->Args({32, 2048})
    ->Args({64, 2048})
    ->ArgNames({"schemas", "schema_bytes"})
    ->Iterations(1)
    ->UseRealTime();

BENCHMARK_CAPTURE(SchemaAdmission, WarmAccepted, SchemaAdmissionShape::warm_accepted)
    ->Args({8, 2048})
    ->Args({32, 2048})
    ->Args({64, 2048})
    ->ArgNames({"schemas", "schema_bytes"})
    ->Iterations(1)
    ->UseRealTime();

BENCHMARK_CAPTURE(SchemaAdmission, Rejected, SchemaAdmissionShape::rejected)
    ->Args({8, 2048})
    ->Args({32, 2048})
    ->Args({64, 2048})
    ->ArgNames({"schemas", "schema_bytes"})
    ->Iterations(1)
    ->UseRealTime();

BENCHMARK_CAPTURE(SchemaAdmission, RetainedGenerations,
                  SchemaAdmissionShape::retained_generations)
    ->Args({8, 2048})
    ->Args({32, 2048})
    ->Args({64, 2048})
    ->ArgNames({"generations", "schema_bytes"})
    ->Iterations(1)
    ->UseRealTime();

BENCHMARK_CAPTURE(HistoryCommit, Unique, HistoryCommitShape::unique)
    ->Args({32, 4096})
    ->Args({256, 512})
    ->Args({2048, 64})
    ->ArgNames({"messages", "message_bytes"})
    ->Iterations(1)
    ->UseRealTime();

BENCHMARK_CAPTURE(HistoryCommit, Aliased, HistoryCommitShape::aliased)
    ->Args({32, 4096})
    ->Args({256, 512})
    ->Args({2048, 64})
    ->ArgNames({"messages", "message_bytes"})
    ->Iterations(1)
    ->UseRealTime();

BENCHMARK_CAPTURE(ToolRegistry, Registration, true)
    ->Arg(8)
    ->Arg(32)
    ->Arg(64)
    ->ArgName("schemas");

} // namespace
} // namespace scry::bench
