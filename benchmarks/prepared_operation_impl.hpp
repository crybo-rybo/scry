#pragma once

#include "scenarios.hpp"

#include <utility>

namespace scry::bench {

template <typename State>
PreparedOperation<State>::PreparedOperation(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

template <typename State> PreparedOperation<State>::~PreparedOperation() = default;

template <typename State>
PreparedOperation<State>::PreparedOperation(PreparedOperation&&) noexcept = default;

template <typename State>
PreparedOperation<State>&
PreparedOperation<State>::operator=(PreparedOperation&&) noexcept = default;

template <typename State> ScenarioResult PreparedOperation<State>::run() {
  if (!state_) {
    return {};
  }
  return state_->run();
}

} // namespace scry::bench
