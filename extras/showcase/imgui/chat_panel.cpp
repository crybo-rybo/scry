#include "chat_panel.hpp"

#include <array>
#include <imgui.h>
#include <optional>
#include <string_view>
#include <utility>

namespace scry_showcase {
namespace {

struct PanelState {
  ChatPhase phase{ChatPhase::idle};
  std::uint64_t generation{};
  std::string user_message{};
  std::string assistant_text{};
  std::string error_message{};
};

class HarnessPanelController final : public PanelController {
public:
  HarnessPanelController(scry::Harness& harness, scry::Conversation& conversation)
      : harness_(harness), conversation_(conversation) {}

  [[nodiscard]] SubmitStatus submit(std::string user_message,
                                    scry::TurnCallbacks callbacks) override {
    auto result =
        harness_.send(conversation_, std::move(user_message), std::move(callbacks));
    if (!result) {
      return std::unexpected(result.error().message);
    }
    turn_.emplace(std::move(*result));
    return {};
  }

  [[nodiscard]] bool cancel() noexcept override {
    return turn_.has_value() && turn_->cancel();
  }

private:
  scry::Harness& harness_;
  scry::Conversation& conversation_;
  std::optional<scry::Turn> turn_{};
};

struct CallbackContext {
  std::shared_ptr<PanelState> state;
  std::uint64_t generation;
};

[[nodiscard]] scry::TurnCallbacks make_callbacks(CallbackContext context) {
  const std::weak_ptr<PanelState> weak_state{context.state};
  const auto generation = context.generation;
  return scry::TurnCallbacks{
      .on_text_delta =
          [weak_state, generation](std::string_view delta) {
            if (const auto locked = weak_state.lock();
                locked && locked->generation == generation) {
              locked->assistant_text.append(delta);
            }
          },
      .on_finished =
          [weak_state, generation](scry::Result<scry::Completion> finished) {
            const auto locked = weak_state.lock();
            if (!locked || locked->generation != generation) {
              return;
            }
            if (finished) {
              if (locked->assistant_text.empty()) {
                locked->assistant_text = std::move(finished->text);
              }
              locked->phase = ChatPhase::completed;
            } else if (finished.error().category == scry::ErrorCategory::cancelled) {
              locked->phase = ChatPhase::cancelled;
            } else {
              locked->error_message = std::move(finished.error().message);
              locked->phase = ChatPhase::failed;
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
  explicit Impl(std::unique_ptr<PanelController> owned_controller)
      : owned_controller_(std::move(owned_controller)), controller_(*owned_controller_),
        state_(std::make_shared<PanelState>()) {}

  explicit Impl(PanelController& controller)
      : controller_(controller), state_(std::make_shared<PanelState>()) {}

  ~Impl() {
    if (can_cancel()) {
      static_cast<void>(controller_.cancel());
    }
  }

  [[nodiscard]] SubmitStatus submit(std::string user_message) {
    if (user_message.empty()) {
      return std::unexpected("Message cannot be empty");
    }
    if (!can_submit()) {
      return std::unexpected("A turn is already active");
    }

    state_->phase = ChatPhase::streaming;
    state_->user_message = user_message;
    state_->assistant_text.clear();
    state_->error_message.clear();
    const auto generation = ++state_->generation;
    auto status = controller_.submit(std::move(user_message),
                                     make_callbacks({state_, generation}));
    if (!status) {
      ++state_->generation;
      state_->phase = ChatPhase::failed;
      state_->error_message = status.error();
    }
    return status;
  }

  [[nodiscard]] bool cancel() noexcept {
    if (!can_cancel()) {
      return false;
    }
    const bool requested = controller_.cancel();
    if (requested && state_->phase == ChatPhase::streaming) {
      state_->phase = ChatPhase::cancelling;
    }
    return requested;
  }

  [[nodiscard]] ChatSnapshot snapshot() const {
    return ChatSnapshot{
        .phase = state_->phase,
        .user_message = state_->user_message,
        .assistant_text = state_->assistant_text,
        .error_message = state_->error_message,
        .can_submit = can_submit(),
        .can_cancel = can_cancel(),
    };
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

  std::unique_ptr<PanelController> owned_controller_{};
  PanelController& controller_;
  std::shared_ptr<PanelState> state_;
  std::array<char, 4096> input_{};
};

ChatPanel::ChatPanel(scry::Harness& harness, scry::Conversation& conversation)
    : impl_(std::make_unique<Impl>(
          std::make_unique<HarnessPanelController>(harness, conversation))) {}

ChatPanel::ChatPanel(PanelController& controller)
    : impl_(std::make_unique<Impl>(controller)) {}

ChatPanel::~ChatPanel() = default;
ChatPanel::ChatPanel(ChatPanel&&) noexcept = default;
ChatPanel& ChatPanel::operator=(ChatPanel&&) noexcept = default;

void ChatPanel::draw() { impl_->draw(); }

SubmitStatus ChatPanel::submit(std::string user_message) {
  return impl_->submit(std::move(user_message));
}

bool ChatPanel::cancel() noexcept { return impl_->cancel(); }

ChatSnapshot ChatPanel::snapshot() const { return impl_->snapshot(); }

} // namespace scry_showcase
