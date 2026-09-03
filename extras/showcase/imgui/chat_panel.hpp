#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <scry/scry.hpp>
#include <string>

namespace scry_showcase {

enum class ChatPhase : std::uint8_t {
  idle,
  streaming,
  cancelling,
  completed,
  failed,
  cancelled,
};

struct ChatSnapshot {
  ChatPhase phase{ChatPhase::idle};
  std::string user_message{};
  std::string assistant_text{};
  std::string error_message{};
  bool can_submit{true};
  bool can_cancel{false};
};

using SubmitStatus = std::expected<void, std::string>;

class PanelController {
public:
  virtual ~PanelController() = default;

  [[nodiscard]] virtual SubmitStatus submit(std::string user_message,
                                            scry::TurnCallbacks callbacks) = 0;
  [[nodiscard]] virtual bool cancel() noexcept = 0;
  /// Stops delivery from the current turn without stopping the turn itself.
  [[nodiscard]] virtual bool disconnect() noexcept = 0;
};

class ChatPanel final {
public:
  ChatPanel(scry::Harness& harness, scry::Conversation& conversation);
  explicit ChatPanel(PanelController& controller);
  ~ChatPanel();

  ChatPanel(ChatPanel&&) noexcept;
  ChatPanel& operator=(ChatPanel&&) noexcept;
  ChatPanel(const ChatPanel&) = delete;
  ChatPanel& operator=(const ChatPanel&) = delete;

  void draw();
  [[nodiscard]] SubmitStatus submit(std::string user_message);
  [[nodiscard]] bool cancel() noexcept;
  [[nodiscard]] ChatSnapshot snapshot() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace scry_showcase
