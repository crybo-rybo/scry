#pragma once

#include <expected>
#include <memory>
#include <scry/scry.hpp>
#include <string>

namespace scry_showcase {

using SubmitStatus = std::expected<void, std::string>;

class ChatPanel final {
public:
  ChatPanel(scry::Harness& harness, scry::Conversation& conversation);
  ~ChatPanel();

  ChatPanel(ChatPanel&&) noexcept;
  ChatPanel& operator=(ChatPanel&&) noexcept;
  ChatPanel(const ChatPanel&) = delete;
  ChatPanel& operator=(const ChatPanel&) = delete;

  void draw();
  [[nodiscard]] SubmitStatus submit(std::string user_message);
  [[nodiscard]] bool cancel() noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace scry_showcase
