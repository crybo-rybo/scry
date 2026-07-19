#pragma once

#include <memory>
#include <scry/config.hpp>
#include <scry/conversation.hpp>
#include <scry/error.hpp>
#include <scry/events.hpp>
#include <scry/tool_registry.hpp>
#include <scry/turn.hpp>
#include <string>

namespace scry {

namespace detail {
class HarnessTestAccess;
} // namespace detail

class Harness final {
public:
  [[nodiscard]] static Result<Harness> create(Config config);

  ~Harness();
  Harness(Harness&&) noexcept;
  Harness& operator=(Harness&&) noexcept;
  Harness(const Harness&) = delete;
  Harness& operator=(const Harness&) = delete;

  [[nodiscard]] ToolRegistry& tools() noexcept;
  [[nodiscard]] const ToolRegistry& tools() const noexcept;

  [[nodiscard]] Result<Turn> send(Conversation& conversation, std::string user_message);

  [[nodiscard]] Result<Completion> send_and_wait(Conversation& conversation,
                                                 std::string user_message);

  UpdateStats update(UpdateOptions options = {});

private:
  class Impl;

  [[nodiscard]] static std::unique_ptr<ToolRegistry> make_tool_registry();

  explicit Harness(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;

  friend class detail::HarnessTestAccess;
};

} // namespace scry
