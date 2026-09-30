// A libFuzzer-compatible corpus replay driver for fuzz targets that only GCC can
// build. It runs every input once through the target's LLVMFuzzerTestOneInput,
// the same deterministic replay a libFuzzer binary performs with -runs=0, without
// coverage guidance or mutation.
//
// Usage: <target> <file-or-directory>...
//
// Directories are walked recursively and their files replayed in sorted path
// order. A crash or sanitizer report fails the process on its own. The driver
// itself exits nonzero when the target returns a value libFuzzer does not accept
// (anything but 0 or -1), when a path cannot be read, or when the paths hold no
// input at all, so a misplaced corpus cannot pass as an empty one.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

namespace fs = std::filesystem;

// Appends the regular files under `path` (or `path` itself) to `inputs`, or
// reports why it cannot.
[[nodiscard]] bool collect_inputs(const fs::path& path, std::vector<fs::path>& inputs) {
  std::error_code error{};
  if (fs::is_regular_file(path, error)) {
    inputs.push_back(path);
    return true;
  }
  if (!fs::is_directory(path, error)) {
    std::cerr << "replay: not a file or directory: " << path << '\n';
    return false;
  }
  std::vector<fs::path> found{};
  for (fs::recursive_directory_iterator it{path, error}, end{}; !error && it != end;
       it.increment(error)) {
    if (it->is_regular_file(error)) {
      found.push_back(it->path());
    }
  }
  if (error) {
    std::cerr << "replay: cannot walk " << path << ": " << error.message() << '\n';
    return false;
  }
  std::ranges::sort(found);
  inputs.insert(inputs.end(), found.begin(), found.end());
  return true;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
read_input(const fs::path& path) {
  std::ifstream stream{path, std::ios::binary};
  if (!stream) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> bytes{};
  std::transform(std::istreambuf_iterator<char>{stream},
                 std::istreambuf_iterator<char>{}, std::back_inserter(bytes),
                 [](const char byte) { return static_cast<std::uint8_t>(byte); });
  if (stream.bad()) {
    return std::nullopt;
  }
  return bytes;
}

// Runs one input; false when it cannot be read or the target rejects the run.
[[nodiscard]] bool replay(const fs::path& path) {
  const auto bytes = read_input(path);
  if (!bytes) {
    std::cerr << "replay: cannot read " << path << '\n';
    return false;
  }
  const auto result = LLVMFuzzerTestOneInput(bytes->data(), bytes->size());
  // libFuzzer accepts 0, and -1 to keep an input out of the corpus; any other
  // value is an error in the target.
  if (result != 0 && result != -1) {
    std::cerr << "replay: target returned " << result << " for " << path << '\n';
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  const auto arguments = std::span{argv, static_cast<std::size_t>(argc)};
  if (arguments.size() < 2) {
    std::cerr << "Usage: " << arguments.front() << " <file-or-directory>...\n";
    return 2;
  }
  std::vector<fs::path> inputs{};
  for (const std::string_view argument : arguments.subspan(1)) {
    if (!collect_inputs(fs::path{argument}, inputs)) {
      return 2;
    }
  }
  if (inputs.empty()) {
    std::cerr << "replay: no inputs found\n";
    return 2;
  }
  const auto failures =
      std::ranges::count_if(inputs, [](const fs::path& path) { return !replay(path); });
  std::cout << "replay: " << inputs.size() - static_cast<std::size_t>(failures)
            << " of " << inputs.size() << " inputs replayed cleanly\n";
  return failures == 0 ? 0 : 1;
}
