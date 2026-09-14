#include "chat_panel.hpp"

#include <array>
#include <cstdint>
#include <imgui.h>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace scry_showcase {
namespace {

enum class ChatPhase : std::uint8_t {
  idle,
  streaming,
  cancelling,
  completed,
  failed,
  cancelled,
};

struct PanelState {
  ChatPhase phase{ChatPhase::idle};
  std::string user_message{};
  std::string assistant_text{};
  std::string error_message{};
};

// The callbacks hold the panel state directly. Nothing here has to ask whether
// the delivery is stale: a superseded or dropped turn is disconnected, so the
// library never invokes these again.
[[nodiscard]] scry::TurnCallbacks make_callbacks(std::shared_ptr<PanelState> state) {
  return scry::TurnCallbacks{
      .on_text_delta =
          [state](std::string_view delta) { state->assistant_text.append(delta); },
      .on_finished =
          [state](scry::Result<scry::Completion> finished) {
            if (finished) {
              if (state->assistant_text.empty()) {
                state->assistant_text = std::move(finished->text);
              }
              state->phase = ChatPhase::completed;
            } else if (finished.error().category == scry::ErrorCategory::cancelled) {
              state->phase = ChatPhase::cancelled;
            } else {
              state->error_message = std::move(finished.error().message);
              state->phase = ChatPhase::failed;
            }
          },
  };
}

[[nodiscard]] const char* phase_label(ChatPhase phase) noexcept {
  switch (phase) {
  case ChatPhase::idle:
    return "Ready";
  case ChatPhase::streaming:
    return "Streaming";
  case ChatPhase::cancelling:
    return "Cancelling";
  case ChatPhase::completed:
    return "Complete";
  case ChatPhase::failed:
    return "Error";
  case ChatPhase::cancelled:
    return "Cancelled";
  }
  return "Unknown";
}

} // namespace

class ChatPanel::Impl final {
public:
  Impl(scry::Harness& harness, scry::Conversation& conversation)
      : harness_(harness), conversation_(conversation),
        state_(std::make_shared<PanelState>()) {}

  ~Impl() {
    if (can_cancel()) {
      static_cast<void>(cancel());
    }
    // The panel is going away; the turn may not be. Disconnecting releases the
    // callbacks holding the panel state, so nothing keeps writing into it.
    disconnect();
  }

  [[nodiscard]] SubmitStatus submit(std::string user_message) {
    if (user_message.empty()) {
      return std::unexpected("Message cannot be empty");
    }
    if (!can_submit()) {
      return std::unexpected("A turn is already active");
    }

    // Whatever a previous turn is still doing, it stops reporting here: the
    // library releases its callbacks, so no stale delivery can reach the panel.
    disconnect();
    state_->phase = ChatPhase::streaming;
    state_->user_message = user_message;
    state_->assistant_text.clear();
    state_->error_message.clear();
    auto result =
        harness_.send(conversation_, std::move(user_message), make_callbacks(state_));
    if (!result) {
      state_->phase = ChatPhase::failed;
      state_->error_message = result.error().message;
      return std::unexpected(state_->error_message);
    }
    turn_.emplace(std::move(*result));
    return {};
  }

  [[nodiscard]] bool cancel() noexcept {
    if (!can_cancel()) {
      return false;
    }
    const bool requested = turn_ && turn_->cancel();
    if (requested && state_->phase == ChatPhase::streaming) {
      state_->phase = ChatPhase::cancelling;
    }
    return requested;
  }

  void draw() {
    ImGui::Begin("Scry chat");
    draw_transcript();
    ImGui::Separator();
    ImGui::InputTextMultiline("##scry-prompt", input_.data(), input_.size(),
                              ImVec2{-1.0F, 84.0F});
    draw_controls();
    ImGui::End();
  }

private:
  void disconnect() noexcept {
    if (turn_) {
      static_cast<void>(turn_->disconnect());
    }
  }

  [[nodiscard]] bool can_submit() const noexcept {
    return state_->phase != ChatPhase::streaming &&
           state_->phase != ChatPhase::cancelling;
  }

  [[nodiscard]] bool can_cancel() const noexcept {
    return state_->phase == ChatPhase::streaming;
  }

  void draw_transcript() const {
    ImGui::Text("Status: %s", phase_label(state_->phase));
    if (!state_->user_message.empty()) {
      ImGui::TextUnformatted("You");
      ImGui::TextWrapped("%s", state_->user_message.c_str());
    }
    if (!state_->assistant_text.empty()) {
      ImGui::TextUnformatted("Assistant");
      ImGui::TextWrapped("%s", state_->assistant_text.c_str());
    }
    if (!state_->error_message.empty()) {
      ImGui::TextUnformatted("Error");
      ImGui::TextWrapped("%s", state_->error_message.c_str());
    }
  }

  void draw_controls() {
    const bool send_enabled = can_submit() && input_.front() != '\0';
    ImGui::BeginDisabled(!send_enabled);
    if (ImGui::Button("Send")) {
      auto status = submit(std::string{input_.data()});
      if (status) {
        input_.fill('\0');
      }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!can_cancel());
    if (ImGui::Button("Cancel")) {
      static_cast<void>(cancel());
    }
    ImGui::EndDisabled();
  }

  scry::Harness& harness_;
  scry::Conversation& conversation_;
  std::optional<scry::Turn> turn_{};
  std::shared_ptr<PanelState> state_;
  std::array<char, 4096> input_{};
};

ChatPanel::ChatPanel(scry::Harness& harness, scry::Conversation& conversation)
    : impl_(std::make_unique<Impl>(harness, conversation)) {}

ChatPanel::~ChatPanel() = default;
ChatPanel::ChatPanel(ChatPanel&&) noexcept = default;
ChatPanel& ChatPanel::operator=(ChatPanel&&) noexcept = default;

void ChatPanel::draw() { impl_->draw(); }

SubmitStatus ChatPanel::submit(std::string user_message) {
  return impl_->submit(std::move(user_message));
}

bool ChatPanel::cancel() noexcept { return impl_->cancel(); }

} // namespace scry_showcase
