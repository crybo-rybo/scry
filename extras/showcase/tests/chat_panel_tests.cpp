#include "chat_panel.hpp"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

struct ControllerState {
  std::vector<scry::TurnCallbacks> submissions{};
  std::vector<std::string> messages{};
  std::string submit_error{};
  std::size_t cancel_requests{};
  std::size_t disconnect_requests{};
  bool cancellation_accepted{true};
};

// The panel sees only what a real turn would deliver through on_finished.
void complete(scry::TurnCallbacks& callbacks, std::string text) {
  callbacks.on_finished(scry::Completion{.text = std::move(text)});
}

void fail(scry::TurnCallbacks& callbacks, std::string message) {
  callbacks.on_finished(std::unexpected(scry::Error{
      .category = scry::ErrorCategory::network,
      .message = std::move(message),
  }));
}

void cancel(scry::TurnCallbacks& callbacks) {
  callbacks.on_finished(std::unexpected(scry::Error{
      .category = scry::ErrorCategory::cancelled,
      .message = "turn cancelled",
  }));
}

class FakeController final : public scry_showcase::PanelController {
public:
  explicit FakeController(std::shared_ptr<ControllerState> state)
      : state_(std::move(state)) {}

  [[nodiscard]] scry_showcase::SubmitStatus
  submit(std::string user_message, scry::TurnCallbacks callbacks) override {
    state_->messages.push_back(std::move(user_message));
    state_->submissions.push_back(std::move(callbacks));
    if (!state_->submit_error.empty()) {
      return std::unexpected(state_->submit_error);
    }
    return {};
  }

  [[nodiscard]] bool cancel() noexcept override {
    ++state_->cancel_requests;
    return state_->cancellation_accepted;
  }

  // Models what the library does: the callbacks of the current turn are
  // released, so a later attempt to deliver through them finds nothing.
  [[nodiscard]] bool disconnect() noexcept override {
    ++state_->disconnect_requests;
    if (state_->submissions.empty()) {
      return false;
    }
    auto& callbacks = state_->submissions.back();
    const bool connected = static_cast<bool>(callbacks.on_finished);
    callbacks = scry::TurnCallbacks{};
    return connected;
  }

private:
  std::shared_ptr<ControllerState> state_;
};

struct PanelFixture {
  std::shared_ptr<ControllerState> controller_state{
      std::make_shared<ControllerState>()};
  FakeController controller{controller_state};
  scry_showcase::ChatPanel panel{controller};
};

} // namespace

TEST_CASE("Chat panel streams a turn and records completion") {
  PanelFixture fixture;

  REQUIRE(fixture.panel.submit("hello"));
  CHECK(fixture.controller_state->messages == std::vector<std::string>{"hello"});
  CHECK(fixture.panel.snapshot().phase == scry_showcase::ChatPhase::streaming);

  auto& callbacks = fixture.controller_state->submissions.back();
  callbacks.on_text_delta("hel");
  callbacks.on_text_delta("lo");
  complete(callbacks, "provider aggregate is ignored after deltas");

  const auto snapshot = fixture.panel.snapshot();
  CHECK(snapshot.phase == scry_showcase::ChatPhase::completed);
  CHECK(snapshot.assistant_text == "hello");
  CHECK(snapshot.can_submit);
  CHECK_FALSE(snapshot.can_cancel);
}

TEST_CASE("Chat panel uses aggregate completion when no deltas arrived") {
  PanelFixture fixture;

  REQUIRE(fixture.panel.submit("hello"));
  complete(fixture.controller_state->submissions.back(), "complete response");

  CHECK(fixture.panel.snapshot().assistant_text == "complete response");
}

TEST_CASE("Chat panel reports synchronous submit errors") {
  PanelFixture fixture;
  fixture.controller_state->submit_error = "provider unavailable";

  const auto status = fixture.panel.submit("hello");

  REQUIRE_FALSE(status);
  CHECK(status.error() == "provider unavailable");
  CHECK(fixture.panel.snapshot().phase == scry_showcase::ChatPhase::failed);
  CHECK(fixture.panel.snapshot().error_message == "provider unavailable");

  // A submission that never became a turn is disconnected, so the callbacks
  // the panel handed over can no longer deliver anything into it.
  CHECK_FALSE(fixture.controller_state->submissions.back().on_finished);
  CHECK(fixture.panel.snapshot().phase == scry_showcase::ChatPhase::failed);
}

TEST_CASE("Chat panel reports asynchronous errors") {
  PanelFixture fixture;

  REQUIRE(fixture.panel.submit("hello"));
  fail(fixture.controller_state->submissions.back(), "stream failed");

  CHECK(fixture.panel.snapshot().phase == scry_showcase::ChatPhase::failed);
  CHECK(fixture.panel.snapshot().error_message == "stream failed");
}

TEST_CASE("Chat panel exposes cancellation state") {
  PanelFixture fixture;

  REQUIRE(fixture.panel.submit("hello"));
  REQUIRE(fixture.panel.cancel());
  CHECK(fixture.panel.snapshot().phase == scry_showcase::ChatPhase::cancelling);
  CHECK_FALSE(fixture.panel.cancel());
  CHECK(fixture.controller_state->cancel_requests == 1);
  cancel(fixture.controller_state->submissions.back());

  CHECK(fixture.panel.snapshot().phase == scry_showcase::ChatPhase::cancelled);
  CHECK(fixture.controller_state->cancel_requests == 1);
}

TEST_CASE("Chat panel validates submission lifecycle") {
  PanelFixture fixture;

  const auto empty_status = fixture.panel.submit("");
  REQUIRE_FALSE(empty_status);
  CHECK(empty_status.error() == "Message cannot be empty");
  REQUIRE(fixture.panel.submit("active"));

  const auto overlapping_status = fixture.panel.submit("overlap");
  REQUIRE_FALSE(overlapping_status);
  CHECK(overlapping_status.error() == "A turn is already active");
  CHECK(fixture.controller_state->submissions.size() == 1);
}

TEST_CASE("Chat panel remains active when cancellation is refused") {
  PanelFixture fixture;
  fixture.controller_state->cancellation_accepted = false;

  REQUIRE(fixture.panel.submit("hello"));
  CHECK_FALSE(fixture.panel.cancel());
  CHECK(fixture.panel.snapshot().phase == scry_showcase::ChatPhase::streaming);
  CHECK(fixture.controller_state->cancel_requests == 1);
}

TEST_CASE("Chat panel disconnects an older submission before starting another") {
  PanelFixture fixture;

  REQUIRE(fixture.panel.submit("first"));
  complete(fixture.controller_state->submissions.front(), "first response");
  REQUIRE(fixture.panel.submit("second"));

  // The older turn was disconnected as the new one started, so it has no
  // callbacks left to deliver a stale outcome through.
  CHECK(fixture.controller_state->disconnect_requests == 2);
  CHECK_FALSE(fixture.controller_state->submissions.front().on_finished);
  CHECK_FALSE(fixture.controller_state->submissions.front().on_text_delta);
  const auto snapshot = fixture.panel.snapshot();
  CHECK(snapshot.phase == scry_showcase::ChatPhase::streaming);
  CHECK(snapshot.user_message == "second");
  CHECK(snapshot.error_message.empty());
}

TEST_CASE("Chat panel destruction cancels and disconnects the live turn") {
  auto controller_state = std::make_shared<ControllerState>();
  FakeController controller{controller_state};
  {
    scry_showcase::ChatPanel panel{controller};
    REQUIRE(panel.submit("hello"));
  }

  CHECK(controller_state->cancel_requests == 1);
  // Nothing can deliver into the destroyed panel: the turn keeps running, but
  // its callbacks are gone.
  CHECK_FALSE(controller_state->submissions.back().on_text_delta);
  CHECK_FALSE(controller_state->submissions.back().on_finished);
}

TEST_CASE("Chat panel destruction cancels only a live streaming turn") {
  auto controller_state = std::make_shared<ControllerState>();
  FakeController controller{controller_state};
  {
    const scry_showcase::ChatPanel panel{controller};
    CHECK(panel.snapshot().phase == scry_showcase::ChatPhase::idle);
  }
  CHECK(controller_state->cancel_requests == 0);

  {
    scry_showcase::ChatPanel panel{controller};
    REQUIRE(panel.submit("completed"));
    complete(controller_state->submissions.back(), "done");
  }
  CHECK(controller_state->cancel_requests == 0);

  {
    scry_showcase::ChatPanel panel{controller};
    REQUIRE(panel.submit("cancelling"));
    REQUIRE(panel.cancel());
  }
  CHECK(controller_state->cancel_requests == 1);
}
