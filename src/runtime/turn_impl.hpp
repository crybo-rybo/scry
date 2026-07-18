#pragma once

#include "runtime/pump.hpp"

#include <memory>
#include <scry/turn.hpp>

namespace scry {

class Turn::Impl final {
public:
  explicit Impl(const std::shared_ptr<detail::TurnRoute>& active_route)
      : turn_id(active_route->id()), cancelled(active_route->cancel_flag()),
        route(active_route) {}

  ~Impl() {
    if (const auto active_route = route.lock()) {
      active_route->detach();
    }
  }

  TurnId turn_id{};
  std::shared_ptr<std::atomic<bool>> cancelled{};
  std::weak_ptr<detail::TurnRoute> route{};
};

} // namespace scry
