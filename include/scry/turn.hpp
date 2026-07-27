#pragma once

#include <memory>
#include <scry/error.hpp>
#include <scry/events.hpp>
#include <scry/turn_id.hpp>

namespace scry {

class Turn final {
public:
  ~Turn();
  Turn(Turn&&) noexcept;
  Turn& operator=(Turn&&) noexcept;
  Turn(const Turn&) = delete;
  Turn& operator=(const Turn&) = delete;

  [[nodiscard]] TurnId id() const noexcept;
  [[nodiscard]] bool cancel() noexcept;

  [[nodiscard]] Status on_text_delta(TextDeltaCallback callback);
  [[nodiscard]] Status on_tool_call(ToolCallCallback callback);
  [[nodiscard]] Status on_completion(CompletionCallback callback);
  [[nodiscard]] Status on_error(ErrorCallback callback);
  [[nodiscard]] Status on_cancelled(CancelledCallback callback);

private:
  class Impl;

  explicit Turn(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;

  friend class Harness;
};

} // namespace scry
