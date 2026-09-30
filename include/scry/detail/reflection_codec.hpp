#pragma once

#include <cstddef>
#include <expected>
#include <scry/error.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace scry::reflection::detail {

// A codec failure on its way out of the recursion. `path` is relative to the value
// the failing call was given (".blocks[2].text") and grows at the front as the
// failure passes each enclosing member or element, so a successful encode or decode
// spends nothing on paths.
struct codec_failure {
  std::string reason{};
  std::string path{};
};

template <typename Type> using codec_result = std::expected<Type, codec_failure>;

[[nodiscard]] inline std::unexpected<codec_failure> codec_fail(std::string reason) {
  return std::unexpected(codec_failure{.reason = std::move(reason)});
}

[[nodiscard]] inline codec_failure at_member(codec_failure failure,
                                             const std::string_view key) {
  std::string prefix{"."};
  prefix.append(key);
  failure.path.insert(0, prefix);
  return failure;
}

[[nodiscard]] inline codec_failure at_element(codec_failure failure,
                                              const std::size_t index) {
  failure.path.insert(0, "[" + std::to_string(index) + "]");
  return failure;
}

// A host-only codec diagnostic. Result encoding uses this shape: the failure
// describes the handler's own result type, whose schema the model never sees,
// so nothing about it is text the model could act on.
[[nodiscard]] inline Error codec_error(const std::string_view path,
                                       const std::string_view message) {
  std::string text{"reflected JSON at "};
  text.append(path);
  text.push_back(' ');
  text.append(message);
  return Error{
      .category = ErrorCategory::tool,
      .message = std::move(text),
  };
}

// An argument-decode diagnostic. Every word of this text is derived from the
// input schema the model was already given, so the model copy is the host copy
// without the library's prefix.
[[nodiscard]] inline Error decode_error(const std::string_view path,
                                        const std::string_view message) {
  auto error = codec_error(path, message);
  error.model_message.assign(path);
  error.model_message.push_back(' ');
  error.model_message.append(message);
  return error;
}

[[nodiscard]] inline Error encode_failure_error(const codec_failure& failure) {
  return codec_error("$" + failure.path, failure.reason);
}

[[nodiscard]] inline Error decode_failure_error(const std::string_view root,
                                                const codec_failure& failure) {
  return decode_error(std::string{root} + failure.path, failure.reason);
}

} // namespace scry::reflection::detail
