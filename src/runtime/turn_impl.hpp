#pragma once

#include "runtime/pump.hpp"

#include <memory>
#include <scry/turn.hpp>

namespace scry {

class Turn::Impl final {
public:
  explicit Impl(const std::shared_ptr<detail::TurnRoute>& active_route)
      : turn_id(active_route->id()), route(active_route) {}

  ~Impl() {
    if (const auto active_route = route.lock()) {
      active_route->detach();
    }
  }

  TurnId turn_id{};
  std::weak_ptr<detail::TurnRoute> route{};
};

} // namespace scry
