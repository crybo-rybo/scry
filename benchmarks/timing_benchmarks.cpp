#include "scenarios.hpp"

#include <benchmark/benchmark.h>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace scry::bench {
namespace {

void publish_result(benchmark::State& state, const ScenarioResult& result) {
  const auto checksum_hi = static_cast<std::uint32_t>(result.digest >> 32U);
  const auto checksum_lo = static_cast<std::uint32_t>(result.digest & 0xffff'ffffULL);
  state.counters["checksum_hi"] = static_cast<double>(checksum_hi);
  state.counters["checksum_lo"] = static_cast<double>(checksum_lo);
  state.counters["input_bytes"] = static_cast<double>(result.input_bytes);
  state.counters["items"] = static_cast<double>(result.items);
  state.counters["output_bytes"] = static_cast<double>(result.output_bytes);
  state.SetBytesProcessed(state.iterations() *
                          static_cast<std::int64_t>(result.input_bytes));
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(result.items));
}

template <typename Factory>
void measure_timing(benchmark::State& state, Factory&& factory) {
  using Scenario = std::invoke_result_t<Factory&>;
  std::optional<Scenario> scenario{};
  ScenarioResult last{};
  for (auto iteration : state) {
    static_cast<void>(iteration);
    if (!scenario) {
      state.PauseTiming();
      scenario.emplace(std::forward<Factory>(factory)());
      last = scenario->run(true);
      benchmark::DoNotOptimize(last.digest);
      state.ResumeTiming();
      if (!last.valid) {
        state.SkipWithError("scenario warm-up validation failed");
        break;
      }
    }
    last = scenario->run(false);
    benchmark::DoNotOptimize(last.items);
    benchmark::DoNotOptimize(last.output_bytes);
    if (!last.valid) {
      state.SkipWithError("scenario semantic validation failed");
      break;
    }
  }
  publish_result(state, last);
}

template <typename Factory>
void measure_prepared_timing(benchmark::State& state, Factory&& factory) {
  using Scenario = std::invoke_result_t<Factory&>;
  using Operation = decltype(std::declval<const Scenario&>().prepare());
  std::optional<Scenario> scenario{};
  ScenarioResult last{};
  for (auto iteration : state) {
    static_cast<void>(iteration);
    state.PauseTiming();
    if (!scenario) {
      scenario.emplace(std::forward<Factory>(factory)());
      last = scenario->validate();
      benchmark::DoNotOptimize(last.digest);
    }
    if (!last.valid) {
      state.SkipWithError("scenario warm-up validation failed");
      break;
    }
    auto operation = std::optional<Operation>{};
    operation.emplace(scenario->prepare());
    state.ResumeTiming();
    last = operation->run();
    benchmark::DoNotOptimize(last.items);
    benchmark::DoNotOptimize(last.output_bytes);
    state.PauseTiming();
    operation.reset();
    if (!last.valid) {
      state.SkipWithError("scenario semantic validation failed");
      break;
    }
    state.ResumeTiming();
  }
  publish_result(state, last);
}

void SSE(benchmark::State& state) {
  const auto chunk_bytes = static_cast<std::size_t>(state.range(0));
  const auto use_crlf = state.range(1) != 0;
  measure_timing(state, [=] { return SseScenario{chunk_bytes, use_crlf}; });
}

void OpenAIStream(benchmark::State& state) {
  const auto shape =
      state.range(0) == 0 ? OpenAiStreamShape::text : OpenAiStreamShape::tools;
  const auto scale = static_cast<std::size_t>(state.range(1));
  measure_timing(state, [=] { return OpenAiStreamScenario{shape, scale}; });
}

void RequestEncoding(benchmark::State& state) {
  const auto dialect =
      state.range(0) == 0 ? RequestDialect::openai : RequestDialect::anthropic;
  const auto message_count = static_cast<std::size_t>(state.range(1));
  const auto schema_count = static_cast<std::size_t>(state.range(2));
  measure_timing(state, [=] {
    return RequestEncodingScenario{dialect, message_count, schema_count};
  });
}

void TurnMachine(benchmark::State& state) {
  const auto tool_count = static_cast<std::size_t>(state.range(0));
  const auto argument_bytes = static_cast<std::size_t>(state.range(1));
  measure_prepared_timing(
      state, [=] { return TurnMachineScenario{tool_count, argument_bytes}; });
}

void Pump(benchmark::State& state) {
  const auto shape =
      state.range(0) == 0 ? PumpShape::text_delivery : PumpShape::completion_commit;
  const auto route_count = static_cast<std::size_t>(state.range(1));
  measure_prepared_timing(state, [=] { return PumpScenario{shape, route_count}; });
}

void Admission(benchmark::State& state, const AdmissionShape shape) {
  const auto element_count = static_cast<std::size_t>(state.range(0));
  const auto element_bytes = static_cast<std::size_t>(state.range(1));
  measure_prepared_timing(
      state, [=] { return AdmissionScenario{shape, element_count, element_bytes}; });
}

void ToolRegistry(benchmark::State& state, const bool /*registration*/) {
  const auto schema_count = static_cast<std::size_t>(state.range(0));
  measure_prepared_timing(state, [=] { return ToolRegistryScenario{schema_count}; });
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

BENCHMARK_CAPTURE(ToolRegistry, Registration, true)
    ->Arg(8)
    ->Arg(32)
    ->Arg(64)
    ->ArgName("schemas");

} // namespace
} // namespace scry::bench
