#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <scry/turn_id.hpp>
#include <scry/unique_function.hpp>
#include <string>
#include <string_view>

namespace scry {

struct ToolCall {
  TurnId turn_id{};
  std::string id{};
  std::string name{};
  Json arguments{};
};

struct Completion {
  TurnId turn_id{};
  std::string text{};
  std::uint32_t attempt_count{};
  std::string provider_request_id{};
};

struct Cancelled {
  TurnId turn_id{};
};

struct UpdateOptions {
  std::optional<std::chrono::microseconds> time_budget{};
  std::size_t max_callbacks{std::numeric_limits<std::size_t>::max()};
};

struct UpdateStats {
  std::size_t callbacks_delivered{};
  std::size_t events_remaining{};
  bool budget_exhausted{false};
};

using TextDeltaCallback = UniqueFunction<void(std::string_view)>;
using ToolCallCallback = UniqueFunction<void(const ToolCall&)>;
using CompletionCallback = UniqueFunction<void(const Completion&)>;
using ErrorCallback = UniqueFunction<void(const Error&)>;
using CancelledCallback = UniqueFunction<void(const Cancelled&)>;

} // namespace scry
