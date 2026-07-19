#pragma once

#include <cstddef>
#include <memory>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <string>

namespace scry {

struct ConversationConfig {
  std::string system_prompt{};
};

class Conversation final {
public:
  [[nodiscard]] static Result<Conversation> create(ConversationConfig config = {});
  [[nodiscard]] static Result<Conversation> from_json(const Json& json);

  ~Conversation();
  Conversation(Conversation&&) noexcept;
  Conversation& operator=(Conversation&&) noexcept;
  Conversation(const Conversation&) = delete;
  Conversation& operator=(const Conversation&) = delete;

  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] std::size_t message_count() const noexcept;
  [[nodiscard]] Result<Json> to_json() const;

private:
  class Impl;

  explicit Conversation(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;

  friend class Harness;
};

} // namespace scry
