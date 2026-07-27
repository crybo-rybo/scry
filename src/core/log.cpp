#include "core/log.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <scry/version.hpp>
#include <string>
#include <string_view>
#include <thread>

namespace scry::detail {
namespace {

constexpr const char* default_log_path = "scry.log";

[[nodiscard]] std::string timestamp_utc() {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::system_clock::to_time_t(now);
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())
          .count() %
      1000;
  std::tm utc{};
  gmtime_r(&seconds, &utc);
  char text[32] = {};
  const auto written = std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%S", &utc);
  return std::format("{}.{:03}Z", std::string_view{text, written}, milliseconds);
}

// A short stable tag distinguishing the app thread from the worker thread in
// interleaved output; not a full thread identifier.
[[nodiscard]] unsigned thread_tag() noexcept {
  const auto hashed = std::hash<std::thread::id>{}(std::this_thread::get_id());
  return static_cast<unsigned>(hashed & 0xFFFFU);
}

struct LogFile {
  LogFile() {
    // The Meyers-singleton constructor runs exactly once, serialized by the
    // language, so the one-shot getenv read is safe here.
    const char* path = std::getenv("SCRY_LOG_FILE"); // NOLINT(concurrency-mt-unsafe)
    stream.open(path != nullptr ? path : default_log_path, std::ios::app);
    if (stream.is_open()) {
      stream << std::format(
          "[{}] [t:{:04x}] Scry {}.{}.{} diagnostic logging started\n", timestamp_utc(),
          thread_tag(), SCRY_VERSION_MAJOR, SCRY_VERSION_MINOR, SCRY_VERSION_PATCH);
      stream.flush();
    }
  }

  std::mutex mutex{};
  std::ofstream stream{};
};

[[nodiscard]] LogFile& log_file() {
  static LogFile instance{};
  return instance;
}

} // namespace

void log_line(const std::string_view message) noexcept {
  try {
    auto& file = log_file();
    const std::scoped_lock lock{file.mutex};
    if (!file.stream.is_open()) {
      return;
    }
    file.stream << std::format("[{}] [t:{:04x}] {}\n", timestamp_utc(), thread_tag(),
                               message);
    file.stream.flush();
  } catch (...) {
    // Diagnostics must never alter library behavior.
  }
}

} // namespace scry::detail
