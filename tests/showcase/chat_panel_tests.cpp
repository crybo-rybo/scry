#include "chat_panel.hpp"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

struct ControllerState {
  std::vector<scry_showcase::PanelCallbacks> submissions{};
  std::vector<std::string> messages{};
  std::string submit_error{};
  std::size_t cancel_requests{};
  bool cancellation_accepted{true};
};

class FakeController final : public scry_showcase::PanelController {
public:
  explicit FakeController(std::shared_ptr<ControllerState> state)
      : state_(std::move(state)) {}

  [[nodiscard]] scry_showcase::SubmitStatus
  submit(std::string user_message, scry_showcase::PanelCallbacks callbacks) override {
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
  callbacks.text_delta("hel");
  callbacks.text_delta("lo");
  callbacks.completed("provider aggregate is ignored after deltas");

  const auto snapshot = fixture.panel.snapshot();
  CHECK(snapshot.phase == scry_showcase::ChatPhase::completed);
  CHECK(snapshot.assistant_text == "hello");
  CHECK(snapshot.can_submit);
  CHECK_FALSE(snapshot.can_cancel);
}

TEST_CASE("Chat panel uses aggregate completion when no deltas arrived") {
  PanelFixture fixture;

  REQUIRE(fixture.panel.submit("hello"));
  fixture.controller_state->submissions.back().completed("complete response");

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

  fixture.controller_state->submissions.back().completed("late completion");
  CHECK(fixture.panel.snapshot().phase == scry_showcase::ChatPhase::failed);
}

TEST_CASE("Chat panel reports asynchronous errors") {
  PanelFixture fixture;

  REQUIRE(fixture.panel.submit("hello"));
  fixture.controller_state->submissions.back().failed("stream failed");

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
  fixture.controller_state->submissions.back().cancelled();

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

TEST_CASE("Chat panel ignores callbacks from an older submission") {
  PanelFixture fixture;

  REQUIRE(fixture.panel.submit("first"));
  fixture.controller_state->submissions.front().completed("first response");
  REQUIRE(fixture.panel.submit("second"));
  fixture.controller_state->submissions.front().failed("stale error");

  const auto snapshot = fixture.panel.snapshot();
  CHECK(snapshot.phase == scry_showcase::ChatPhase::streaming);
  CHECK(snapshot.user_message == "second");
  CHECK(snapshot.error_message.empty());
}

TEST_CASE("Chat panel destruction requests cancellation without retaining state") {
  auto controller_state = std::make_shared<ControllerState>();
  FakeController controller{controller_state};
  {
    scry_showcase::ChatPanel panel{controller};
    REQUIRE(panel.submit("hello"));
  }

  CHECK(controller_state->cancel_requests == 1);
  controller_state->submissions.back().text_delta("late");
  controller_state->submissions.back().cancelled();
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
    controller_state->submissions.back().completed("done");
  }
  CHECK(controller_state->cancel_requests == 0);

  {
    scry_showcase::ChatPanel panel{controller};
    REQUIRE(panel.submit("cancelling"));
    REQUIRE(panel.cancel());
  }
  CHECK(controller_state->cancel_requests == 1);
}
