#include "tool_codec.hpp"

#include <cmath>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <utility>

namespace scry_tool_chat {
namespace tool_codec_detail {

struct WireArithmeticArguments {
  std::optional<double> a{};
  std::optional<double> b{};
};

} // namespace tool_codec_detail

namespace {

[[nodiscard]] scry::Error codec_error(std::string message) {
  return scry::Error{
      .category = scry::ErrorCategory::tool,
      .message = std::move(message),
  };
}

} // namespace

scry::Result<ArithmeticArguments> decode_arithmetic_arguments(const scry::Json& input) {
  tool_codec_detail::WireArithmeticArguments decoded{};
  if (glz::read_json(decoded, input.text) || !decoded.a || !decoded.b ||
      !std::isfinite(*decoded.a) || !std::isfinite(*decoded.b)) {
    return std::unexpected(
        codec_error("arithmetic tools require finite numeric a and b fields"));
  }
  return ArithmeticArguments{.a = *decoded.a, .b = *decoded.b};
}

scry::Result<scry::Json> encode_arithmetic_result(const ArithmeticResult& result) {
  if (!std::isfinite(result.result)) {
    return std::unexpected(codec_error("arithmetic result is not finite"));
  }
  auto encoded = glz::write_json(result);
  if (!encoded) {
    return std::unexpected(codec_error("arithmetic result could not be encoded"));
  }
  return scry::Json{.text = std::move(*encoded)};
}

} // namespace scry_tool_chat
