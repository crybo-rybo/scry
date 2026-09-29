// The llamad dialect end to end: the public API against a real llamad daemon
// serving a real model. scripts/e2e-llamad.sh builds and runs this; see it for
// usage. The suite starts its own daemon on a private socket and stops,
// restarts, or kills it where a case needs to.
//
// Environment:
//   SCRY_E2E_LLAMAD_BINARY  the llamad executable
//   SCRY_E2E_LLAMAD_MODEL   a GGUF chat model
//   SCRY_E2E_LLAMAD_LOG     where the daemon's stderr goes (default llamad-e2e.log)
// Every case skips when the first two are unset.
//
// Cases tagged [tools] need a model that calls tools when asked; the rest hold
// for any chat model. Assertions check what docs/architecture.md promises,
// never the model's wording.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <optional>
#include <scry/scry.hpp>
#include <spawn.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

extern char** environ;

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;
using namespace std::chrono_literals;

// --- the daemon ------------------------------------------------------------

// One `[llamad] Chat ...` line of the daemon's log.
struct ChatStats {
  int prompt_tokens{};
  int cached_prompt_tokens{};
  int completion_tokens{};
};

[[nodiscard]] std::string environment(const char* name, std::string fallback = {}) {
  const char* value = std::getenv(name);
  return value == nullptr || *value == '\0' ? fallback : std::string{value};
}

// A llamad process on a private socket. llamad loads its model before it
// listens, so a socket that accepts a connection is a daemon ready to serve.
class Daemon final {
public:
  Daemon(std::string binary, std::string model)
      : binary_(std::move(binary)), model_(std::move(model)),
        socket_("/tmp/scry-e2e-" + std::to_string(::getpid()) + ".sock"),
        log_(environment("SCRY_E2E_LLAMAD_LOG", "llamad-e2e.log")) {}

  Daemon(const Daemon&) = delete;
  Daemon& operator=(const Daemon&) = delete;
  ~Daemon() { stop(SIGTERM); }

  [[nodiscard]] std::string target() const { return "unix:" + socket_; }

  // Starts the daemon unless it is running, and waits until it listens.
  void start() {
    if (pid_ != -1) {
      return;
    }
    posix_spawn_file_actions_t actions{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, log_.c_str(),
                                     O_WRONLY | O_CREAT | O_APPEND, 0644);
    std::array<std::string, 7> args{binary_, "--model", model_, "--socket",
                                    socket_, "--ctx",   "8192"};
    std::array<char*, args.size() + 1> argv{};
    for (std::size_t i = 0; i < args.size(); ++i) {
      argv[i] = args[i].data();
    }
    const int spawned =
        posix_spawn(&pid_, binary_.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) {
      pid_ = -1;
      throw std::runtime_error{"could not start " + binary_};
    }
    wait_until_listening();
  }

  // Stops the daemon with `signal` and reaps it; SIGKILL leaves no chance to
  // finish a stream or remove the socket.
  void stop(const int signal) {
    if (pid_ == -1) {
      return;
    }
    ::kill(pid_, signal);
    ::waitpid(pid_, nullptr, 0);
    pid_ = -1;
  }

  [[nodiscard]] std::size_t log_size() const {
    std::ifstream log{log_, std::ios::ate};
    return log ? static_cast<std::size_t>(log.tellg()) : 0;
  }

  // The last `count` finished Chat lines logged after `offset`, once there are
  // that many. The daemon logs a request after its stream ends, so a client
  // can see its final chunk a moment before the line appears, and a request
  // whose client left, perhaps in an earlier case, is logged as CANCELLED once
  // the daemon notices. Those are skipped.
  [[nodiscard]] std::vector<ChatStats>
  finished_chats_since(const std::size_t offset, const std::size_t count) const {
    std::vector<ChatStats> chats;
    for (const auto deadline = Clock::now() + 5s; Clock::now() < deadline;
         std::this_thread::sleep_for(20ms)) {
      chats = read_finished_chats(offset);
      if (chats.size() >= count) {
        chats.erase(chats.begin(), chats.end() - static_cast<std::ptrdiff_t>(count));
        break;
      }
    }
    return chats;
  }

private:
  void wait_until_listening() {
    for (const auto deadline = Clock::now() + 600s; Clock::now() < deadline;
         std::this_thread::sleep_for(100ms)) {
      int status = 0;
      if (::waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        throw std::runtime_error{"llamad exited during startup; see " + log_};
      }
      if (accepts_connection()) {
        return;
      }
    }
    stop(SIGKILL);
    throw std::runtime_error{"llamad did not listen within 600 s; see " + log_};
  }

  [[nodiscard]] bool accepts_connection() const {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    socket_.copy(address.sun_path, sizeof(address.sun_path) - 1);
    const bool connected = ::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                                     sizeof(address)) == 0;
    ::close(fd);
    return connected;
  }

  [[nodiscard]] std::vector<ChatStats>
  read_finished_chats(const std::size_t offset) const {
    std::ifstream log{log_};
    log.seekg(static_cast<std::streamoff>(offset));
    std::vector<ChatStats> chats;
    for (std::string line; std::getline(log, line);) {
      ChatStats stats;
      std::array<char, 16> finish{};
      if (std::sscanf(line.c_str(),
                      "[llamad] Chat prompt_tokens=%d cached_prompt_tokens=%d "
                      "completion_tokens=%d finish=%15s",
                      &stats.prompt_tokens, &stats.cached_prompt_tokens,
                      &stats.completion_tokens, finish.data()) == 4 &&
          std::string_view{finish.data()} != "CANCELLED") {
        chats.push_back(stats);
      }
    }
    return chats;
  }

  std::string binary_;
  std::string model_;
  std::string socket_;
  std::string log_;
  pid_t pid_{-1};
};

// The suite's one daemon, started on first use and kept across cases.
[[nodiscard]] Daemon& daemon() {
  static const auto binary = environment("SCRY_E2E_LLAMAD_BINARY");
  static const auto model = environment("SCRY_E2E_LLAMAD_MODEL");
  if (binary.empty() || model.empty()) {
    SKIP("SCRY_E2E_LLAMAD_BINARY and SCRY_E2E_LLAMAD_MODEL are unset");
  }
  static Daemon instance{binary, model};
  instance.start();
  return instance;
}

// --- Scry ------------------------------------------------------------------

[[nodiscard]] scry::Config config() {
  return {
      .base_url = daemon().target(),
      .model = "local",
      .dialect = scry::ProviderDialect::llamad,
      .sampling = {.temperature = 0.0, .max_tokens = 1024},
  };
}

[[nodiscard]] scry::Harness harness(scry::Config config) {
  auto created = scry::Harness::create(std::move(config));
  REQUIRE(created);
  return std::move(*created);
}

[[nodiscard]] scry::Conversation conversation(std::string system_prompt = {}) {
  auto created =
      scry::Conversation::create({.system_prompt = std::move(system_prompt)});
  REQUIRE(created);
  return std::move(*created);
}

[[nodiscard]] std::string describe(const scry::Result<scry::Completion>& result) {
  std::ostringstream out;
  if (result) {
    out << "completion: finish=" << static_cast<int>(result->finish_reason)
        << " usage=" << result->usage.input_tokens << '/' << result->usage.output_tokens
        << " attempts=" << result->attempt_count
        << " rounds=" << result->tool_round_count << " text=" << result->text;
  } else {
    const auto& error = result.error();
    out << "error: category=" << static_cast<int>(error.category)
        << " retryable=" << error.retryable << " attempt=" << error.attempt
        << " message=" << error.message << " detail=" << error.provider_detail;
  }
  return out.str();
}

struct TurnOutcome {
  scry::Result<scry::Completion> result{std::unexpected(scry::Error{})};
  std::string streamed{};
  std::size_t tool_calls_observed{};
  Clock::time_point finished{};
};

// Sends one turn and pumps until it finishes, running `on_first_delta` once
// when the first text arrives.
[[nodiscard]] TurnOutcome
run_turn(scry::Harness& harness, scry::Conversation& conversation, std::string message,
         std::function<void(scry::Turn&)> on_first_delta = {}) {
  TurnOutcome outcome;
  bool done = false;
  std::optional<scry::Turn> turn;
  auto sent = harness.send(
      conversation, std::move(message),
      {
          .on_text_delta =
              [&](const std::string_view delta) {
                const bool first = outcome.streamed.empty();
                outcome.streamed += delta;
                if (first && on_first_delta) {
                  on_first_delta(*turn);
                }
              },
          .on_tool_call = [&](const scry::ToolCall&) { ++outcome.tool_calls_observed; },
          .on_finished =
              [&](scry::Result<scry::Completion> result) {
                outcome.finished = Clock::now();
                outcome.result = std::move(result);
                done = true;
              },
      });
  REQUIRE(sent);
  turn.emplace(std::move(*sent));
  while (!done) {
    harness.update();
    std::this_thread::sleep_for(1ms);
  }
  UNSCOPED_INFO(describe(outcome.result));
  return outcome;
}

void require_error(const TurnOutcome& outcome, const scry::ErrorCategory category) {
  REQUIRE_FALSE(outcome.result.has_value());
  CHECK(outcome.result.error().category == category);
}

// Long enough to still be streaming when a case cancels, times out, or kills it.
constexpr std::string_view long_reply =
    "Write a detailed 800-word story about a lighthouse keeper.";

// About 4,000 tokens: decoding it takes longer than a tight idle bound on any
// hardware, and it fits the suite's 8,192-token context.
[[nodiscard]] std::string long_prompt() {
  std::string text = "Here is a log. Reply with only the number of its last line.\n";
  for (int line = 1; line <= 200; ++line) {
    text += "Line " + std::to_string(line) +
            ": the quick brown fox jumps over the lazy dog near the river bank.\n";
  }
  return text;
}

struct ZoneArgs {
  [[= scry::reflection::description{
      "IANA time zone, such as Europe/Oslo"}]] std::string timezone;
};

struct TimeReading {
  std::string timezone;
  std::string time;
};

constexpr std::string_view oslo_time = "2026-09-28 22:19:45 CEST";

[[nodiscard]] TimeReading read_clock(ZoneArgs args) {
  return {.timezone = std::move(args.timezone), .time = std::string{oslo_time}};
}

const scry::reflection::ToolMetadata clock_tool{
    .name = "get_current_time", .description = "Get the current time in a time zone"};

} // namespace

// --- chat ------------------------------------------------------------------

TEST_CASE("plain chat streams text and reports the daemon's token counts",
          "[llamad][chat]") {
  auto agent = harness(config());
  auto history = conversation();
  const auto offset = daemon().log_size();
  const auto outcome = run_turn(agent, history, "Hello");

  REQUIRE(outcome.result);
  CHECK(outcome.result->finish_reason == scry::FinishReason::completed);
  CHECK(outcome.result->attempt_count == 1);
  // The pump may coalesce deltas, but never drops or reorders text.
  CHECK(outcome.streamed == outcome.result->text);
  const auto chats = daemon().finished_chats_since(offset, 1);
  REQUIRE(chats.size() == 1);
  CHECK(outcome.result->usage.input_tokens ==
        static_cast<std::uint64_t>(chats[0].prompt_tokens));
  CHECK(outcome.result->usage.output_tokens ==
        static_cast<std::uint64_t>(chats[0].completion_tokens));
}

TEST_CASE("a multi-turn conversation reuses the daemon's prompt cache",
          "[llamad][chat]") {
  auto agent = harness(config());
  auto history =
      conversation("You are a concise assistant in a three-turn test. /no_think");
  const auto offset = daemon().log_size();
  for (const auto* message : {"My name is Ada. Reply with one short sentence.",
                              "What is 2 + 3?", "What is my name?"}) {
    REQUIRE(run_turn(agent, history, message).result);
  }

  CHECK(history.message_count() == 6);
  const auto chats = daemon().finished_chats_since(offset, 3);
  REQUIRE(chats.size() == 3);
  // Scry resends the same history, so each turn's prompt extends the last.
  CHECK(chats[1].cached_prompt_tokens > chats[0].cached_prompt_tokens);
  CHECK(chats[2].cached_prompt_tokens > chats[1].cached_prompt_tokens);
}

TEST_CASE("max_tokens ends the reply as length", "[llamad][chat]") {
  auto small = config();
  small.sampling.max_tokens = 8;
  auto agent = harness(std::move(small));
  auto history = conversation();
  const auto outcome = run_turn(agent, history, "Write a poem about the sea.");

  REQUIRE(outcome.result);
  CHECK(outcome.result->finish_reason == scry::FinishReason::length);
  CHECK(outcome.result->usage.output_tokens == 8);
}

TEST_CASE("two Conversations on one Harness run in FIFO order", "[llamad][chat]") {
  auto agent = harness(config());
  auto first = conversation("/no_think");
  auto second = conversation("/no_think");
  std::vector<std::string> finished;
  const auto record = [&finished](std::string label) {
    return scry::TurnCallbacks{
        .on_finished =
            [&finished, label](scry::Result<scry::Completion> result) {
              finished.push_back(result ? label : label + " failed");
            },
    };
  };
  auto turn_a = agent.send(first, "Name a colour.", record("A"));
  auto turn_b = agent.send(second, "Name a fruit.", record("B"));
  REQUIRE(turn_a);
  REQUIRE(turn_b);
  while (finished.size() < 2) {
    agent.update();
    std::this_thread::sleep_for(1ms);
  }

  CHECK(finished == std::vector<std::string>{"A", "B"});
}

// --- tools -----------------------------------------------------------------

TEST_CASE("one tool round reaches the model and back", "[llamad][tools]") {
  auto agent = harness(config());
  REQUIRE(scry::reflection::add<ZoneArgs>(agent.tools(), clock_tool, read_clock));
  auto history = conversation();
  const auto outcome = run_turn(agent, history, "What time is it in Oslo?");

  REQUIRE(outcome.result);
  CHECK(outcome.result->finish_reason == scry::FinishReason::completed);
  CHECK(outcome.tool_calls_observed == 1);
  CHECK(outcome.result->tool_round_count == 1);
  CHECK(outcome.result->text.find("22:19") != std::string::npos);
  // user, assistant with the call, user with the result, final assistant
  REQUIRE(history.message_count() == 4);
  const auto& messages = history.messages();
  CHECK(std::holds_alternative<scry::ToolCallBlock>(messages[1].content.back()));
  CHECK(std::holds_alternative<scry::ToolResultBlock>(messages[2].content.front()));
}

TEST_CASE("a tool error reaches the model, which calls again", "[llamad][tools]") {
  auto agent = harness(config());
  int calls = 0;
  REQUIRE(scry::reflection::add<ZoneArgs>(
      agent.tools(), clock_tool, [&calls](ZoneArgs args) -> scry::Result<TimeReading> {
        if (++calls == 1) {
          return std::unexpected(
              scry::tool_error("The time service is briefly unavailable. Call the "
                               "tool again with the same arguments."));
        }
        return read_clock(std::move(args));
      }));
  auto history = conversation();
  const auto outcome = run_turn(agent, history, "What time is it in Oslo?");

  REQUIRE(outcome.result);
  CHECK(calls == 2);
  CHECK(outcome.result->tool_round_count == 2);
  CHECK(outcome.result->rejected_tool_call_count == 0);
  const auto& failed =
      std::get<scry::ToolResultBlock>(history.messages()[2].content[0]);
  CHECK(failed.is_error);
  CHECK(failed.result.text.starts_with(R"({"error":)"));
}

struct CityArgs {
  [[= scry::reflection::description{"City name, such as Oslo"}]] std::string city;
};
struct CityCode {
  std::string code;
};
struct CodeArgs {
  [[= scry::reflection::description{
      "City code returned by lookup_city_code"}]] std::string code;
};
struct Weather {
  std::string summary;
};

TEST_CASE("the tool-round limit completes with the unexecuted calls",
          "[llamad][tools]") {
  auto limited = config();
  limited.max_tool_rounds = 1;
  limited.tool_round_limit = scry::ToolRoundLimitPolicy::complete;
  auto agent = harness(std::move(limited));
  REQUIRE(scry::reflection::add<CityArgs>(
      agent.tools(),
      {.name = "lookup_city_code",
       .description = "Look up the opaque city code that get_weather requires"},
      [](CityArgs) { return CityCode{.code = "C-4471"}; }));
  REQUIRE(scry::reflection::add<CodeArgs>(
      agent.tools(),
      {.name = "get_weather",
       .description = "Get the weather for a city code from lookup_city_code"},
      [](CodeArgs) { return Weather{.summary = "Light rain, 9 C"}; }));
  auto history = conversation();
  const auto outcome = run_turn(agent, history,
                                "What is the weather in Oslo? You must first look up "
                                "its city code, then call get_weather with that code.");

  REQUIRE(outcome.result);
  CHECK(outcome.result->finish_reason == scry::FinishReason::tool_round_limit);
  REQUIRE_FALSE(outcome.result->unexecuted_tool_calls.empty());
  CHECK(outcome.result->unexecuted_tool_calls[0].name == "get_weather");
}

// --- cancellation and timeouts ---------------------------------------------

TEST_CASE("cancelling mid-stream finishes promptly and commits nothing",
          "[llamad][cancel]") {
  auto agent = harness(config());
  auto history = conversation();
  Clock::time_point cancelled{};
  const auto outcome =
      run_turn(agent, history, std::string{long_reply}, [&cancelled](scry::Turn& turn) {
        cancelled = Clock::now();
        turn.cancel();
      });

  require_error(outcome, scry::ErrorCategory::cancelled);
  CHECK(history.message_count() == 0);
  CHECK(outcome.finished - cancelled < 2s); // timeouts.shutdown
}

TEST_CASE("destroying the Harness mid-stream returns promptly", "[llamad][cancel]") {
  std::optional<scry::Harness> agent{harness(config())};
  auto history = conversation();
  bool streaming = false;
  auto turn = agent->send(
      history, std::string{long_reply},
      {.on_text_delta = [&streaming](std::string_view) { streaming = true; }});
  REQUIRE(turn);
  while (!streaming) {
    agent->update();
    std::this_thread::sleep_for(1ms);
  }
  const auto start = Clock::now();
  agent.reset();

  CHECK(Clock::now() - start < 2500ms); // about timeouts.shutdown
  CHECK_FALSE(history.busy());
}

TEST_CASE("an idle bound shorter than the prompt's decode fails, and so does "
          "its retry",
          "[llamad][timeouts]") {
  auto tight = config();
  tight.timeouts.idle = 50ms;
  tight.retry.max_attempts = 2;
  auto agent = harness(std::move(tight));
  auto history = conversation("/no_think");
  const auto timed_out = run_turn(agent, history, long_prompt());

  require_error(timed_out, scry::ErrorCategory::network);
  CHECK(timed_out.result.error().provider_detail == "llamad:idle_timeout");
  CHECK(timed_out.result.error().retryable);
  CHECK(timed_out.result.error().attempt == 2);

  // The default idle bound covers the decode. The daemon keeps decoding a
  // prompt whose client left (crybo-rybo/llamad#25), so this turn also waits
  // for the two abandoned attempts.
  auto patient = harness(config());
  CHECK(run_turn(patient, history, long_prompt()).result);
}

TEST_CASE("the transfer bound is the call's deadline", "[llamad][timeouts]") {
  auto bounded = config();
  bounded.timeouts.transfer = 1500ms;
  auto agent = harness(std::move(bounded));
  auto history = conversation();
  const auto outcome = run_turn(agent, history, std::string{long_reply});

  require_error(outcome, scry::ErrorCategory::network);
  CHECK(outcome.result.error().provider_detail == "llamad:deadline_exceeded");
}

// --- request limits --------------------------------------------------------

TEST_CASE("a request over gRPC's 4 MiB message limit is a resource limit",
          "[llamad][limits]") {
  auto agent = harness(config());
  auto history = conversation();
  const auto outcome =
      run_turn(agent, history, std::string(std::size_t{4} * 1024 * 1024 + 4096, 'a'));

  require_error(outcome, scry::ErrorCategory::resource_limit);
  CHECK(outcome.result.error().provider_detail == "llamad:resource_exhausted");
}

TEST_CASE("a prompt longer than the daemon's context is rejected", "[llamad][limits]") {
  auto agent = harness(config());
  auto history = conversation();
  std::string words;
  for (int word = 0; word < 12'000; ++word) {
    words += "word" + std::to_string(word) + ' ';
  }
  const auto outcome = run_turn(agent, history, std::move(words));

  require_error(outcome, scry::ErrorCategory::protocol);
  CHECK(outcome.result.error().message == "llamad rejected the request");
  CHECK(outcome.result.error().provider_detail == "llamad:invalid_argument");
}

// --- the daemon's lifecycle ------------------------------------------------

TEST_CASE("an absent daemon fails fast as unreachable and is retried",
          "[llamad][lifecycle]") {
  auto absent = config();
  absent.base_url = "unix:/tmp/scry-e2e-nobody-" + std::to_string(::getpid()) + ".sock";
  auto agent = harness(std::move(absent));
  auto history = conversation();
  const auto start = Clock::now();
  const auto outcome = run_turn(agent, history, "Hello");

  require_error(outcome, scry::ErrorCategory::network);
  CHECK(outcome.result.error().message == "llamad daemon is unreachable");
  CHECK(outcome.result.error().retryable);
  CHECK(outcome.result.error().attempt == 3);
  CHECK(Clock::now() - start < 3s); // two backoffs of at most 300 and 600 ms
}

TEST_CASE("a restarted daemon serves the next turn on the same Harness",
          "[llamad][lifecycle]") {
  auto agent = harness(config());
  auto history = conversation("/no_think");
  REQUIRE(run_turn(agent, history, "Say hi.").result);
  daemon().stop(SIGTERM);
  daemon().start();

  CHECK(run_turn(agent, history, "Say bye.").result);
}

TEST_CASE("a turn sent inside the reconnect window succeeds with a one-second "
          "initial backoff",
          "[llamad][lifecycle]") {
  // docs/architecture.md: after a failed connection the channel reports failure
  // for up to a second, which a RetryPolicy with this initial backoff rides out.
  auto patient = config();
  patient.retry.initial_backoff = 1s;
  auto agent = harness(std::move(patient));
  auto history = conversation("/no_think");
  daemon().stop(SIGTERM);
  require_error(run_turn(agent, history, "Say hi."), scry::ErrorCategory::network);
  daemon().start();

  CHECK(run_turn(agent, history, "Say hi.").result);
}

TEST_CASE("a daemon killed mid-stream fails the turn without retrying or committing",
          "[llamad][lifecycle]") {
  const int signal = GENERATE(SIGTERM, SIGKILL);
  CAPTURE(signal);
  auto agent = harness(config());
  auto history = conversation();
  const auto outcome = run_turn(agent, history, std::string{long_reply},
                                [signal](scry::Turn&) { daemon().stop(signal); });
  daemon().start();

  require_error(outcome, scry::ErrorCategory::network);
  CHECK(outcome.result.error().retryable);
  CHECK(outcome.result.error().attempt == 1); // text had streamed
  CHECK(history.message_count() == 0);
}
