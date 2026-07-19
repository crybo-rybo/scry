#include "chat_panel.hpp"

#include <array>
#include <imgui.h>
#include <optional>
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

[[nodiscard]] SubmitStatus registration_status(const scry::Status& status) {
  if (status) {
    return {};
  }
  return std::unexpected(status.error().message);
}

[[nodiscard]] SubmitStatus attach_callbacks(scry::Turn& turn,
                                            PanelCallbacks callbacks) {
  auto status = turn.on_text_delta([callback = std::move(callbacks.text_delta)](
                                       std::string_view delta) { callback(delta); });
  if (!status) {
    return registration_status(status);
  }
  status =
      turn.on_complete([callback = std::move(callbacks.completed)](
                           const scry::Completion& value) { callback(value.text); });
  if (!status) {
    return registration_status(status);
  }
  status = turn.on_error([callback = std::move(callbacks.failed)](
                             const scry::Error& error) { callback(error.message); });
  if (!status) {
    return registration_status(status);
  }
  status = turn.on_cancelled([callback = std::move(callbacks.cancelled)](
                                 const scry::Cancelled&) { callback(); });
  return registration_status(status);
}

class HarnessPanelController final : public PanelController {
public:
  HarnessPanelController(scry::Harness& harness, scry::Conversation& conversation)
      : harness_(harness), conversation_(conversation) {}

  [[nodiscard]] SubmitStatus submit(std::string user_message,
                                    PanelCallbacks callbacks) override {
    auto result = harness_.send(conversation_, std::move(user_message));
    if (!result) {
      return std::unexpected(result.error().message);
    }

    auto turn = std::move(*result);
    auto status = attach_callbacks(turn, std::move(callbacks));
    if (!status) {
      static_cast<void>(turn.cancel());
      return status;
    }
    turn_.emplace(std::move(turn));
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

[[nodiscard]] PanelCallbacks make_callbacks(CallbackContext context) {
  const std::weak_ptr<PanelState> weak_state{context.state};
  const auto generation = context.generation;
  return PanelCallbacks{
      .text_delta =
          [weak_state, generation](std::string_view delta) {
            if (const auto locked = weak_state.lock();
                locked && locked->generation == generation) {
              locked->assistant_text.append(delta);
            }
          },
      .completed =
          [weak_state, generation](std::string text) {
            if (const auto locked = weak_state.lock();
                locked && locked->generation == generation) {
              if (locked->assistant_text.empty()) {
                locked->assistant_text = std::move(text);
              }
              locked->phase = ChatPhase::completed;
            }
          },
      .failed =
          [weak_state, generation](std::string message) {
            if (const auto locked = weak_state.lock();
                locked && locked->generation == generation) {
              locked->error_message = std::move(message);
              locked->phase = ChatPhase::failed;
            }
          },
      .cancelled =
          [weak_state, generation] {
            if (const auto locked = weak_state.lock();
                locked && locked->generation == generation) {
              locked->phase = ChatPhase::cancelled;
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
