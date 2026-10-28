#include "kernel/kernel.hpp"

#include "kernel/json/codec.hpp"

#include "kernel/error.hpp"

#include <scrydantic/detail/json_text.hpp>
#include <scrydantic/json.hpp>
#include <string>
#include <utility>

namespace scry::detail {
namespace {

[[nodiscard]] Error codec_error(const ErrorCategory category,
                                const std::string_view failure_message) {
  return make_error(category, std::string{failure_message});
}

} // namespace

Status validate_json(const std::string_view input, const ErrorCategory category,
                     const std::string_view failure_message) {
  if (!scrydantic::detail::validate_json(input)) {
    return std::unexpected(codec_error(category, failure_message));
  }
  return {};
}

Status validate_json_object(const std::string_view input, const ErrorCategory category,
                            const std::string_view failure_message) {
  if (!scrydantic::detail::validate_json_object(input)) {
    return std::unexpected(codec_error(category, failure_message));
  }
  return {};
}

Result<scrydantic::Json> canonicalize_json(const scrydantic::Json& json,
                                           const ErrorCategory category,
                                           const std::string_view failure_message) {
  auto canonical = scrydantic::detail::canonicalize_json(json);
  if (!canonical) {
    return std::unexpected(codec_error(category, failure_message));
  }
  return std::move(*canonical);
}

Result<scrydantic::Json>
canonicalize_json_object(const scrydantic::Json& json, const ErrorCategory category,
                         const std::string_view failure_message) {
  auto canonical = scrydantic::detail::canonicalize_json_object(json);
  if (!canonical) {
    return std::unexpected(codec_error(category, failure_message));
  }
  return std::move(*canonical);
}

} // namespace scry::detail
