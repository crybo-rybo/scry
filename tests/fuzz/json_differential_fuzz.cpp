// Differential fuzz target: the Glaze-backed codec (kernel/json/codec.hpp and
// glaze_document.hpp) against Scry's own JSON layer (kernel/json/document.hpp).
// It exists only while both do, to prove the replacement before Glaze is
// removed; tests/kernel/json_fixture_tests keeps what it learned.
//
// For every input it asserts that
//   - the new validator accepts exactly what the new parser accepts;
//   - the old and new parsers accept exactly the same inputs;
//   - for an accepted input, every node of the two trees has the same kind and
//     the same value (doubles bit for bit), and the two canonical writers
//     produce the same bytes;
//   - quoting the raw input as a string gives the same bytes both ways.
//
// The new layer departs from Glaze deliberately in two places, and only there
// (docs in kernel/json/document.hpp). Both are about which kind a number gets,
// and both are applied here by rewriting the input the old parser sees, so that
// every other byte and node is still compared strictly:
//   1. A number with an exponent is a double even when its value is whole
//      (1e2, -5E+3, 0e9); Glaze made it an integer when it fitted. Without this
//      Glaze's canonical form is not idempotent: 1.0e19 writes as 1E19, which
//      reads back as the integer 10000000000000000000. The oracle sees such a
//      token with ".0" added to its mantissa, which Glaze reads as a double.
//   2. "-0" is the double -0.0; Glaze read it as the signed integer 0, so its
//      canonical form was not idempotent either (-0.0 writes as -0, which read
//      back as 0). The oracle sees it as "-0.0".

#include "kernel/json/codec.hpp"
#include "kernel/json/document.hpp"
#include "kernel/json/glaze_document.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <string>
#include <string_view>

namespace {

namespace json = scry::detail::json;
using scry::detail::JsonValue;

[[noreturn]] void fail(const std::string_view what, const std::string_view input) {
  std::fprintf(stderr, "json differential: %.*s\ninput (%zu bytes): ",
               static_cast<int>(what.size()), what.data(), input.size());
  for (const char character : input) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte >= 0x20U && byte < 0x7FU && byte != '%') {
      std::fputc(byte, stderr);
    } else {
      std::fprintf(stderr, "%%%02X", byte);
    }
  }
  std::fputc('\n', stderr);
  std::abort();
}

[[nodiscard]] bool is_number_character(const char character) {
  return (character >= '0' && character <= '9') || character == '-' ||
         character == '+' || character == '.' || character == 'e' || character == 'E';
}

// Divergence 1 and 2 above, as a rewrite of one number token.
void append_oracle_number(std::string& output, const std::string_view token) {
  const auto exponent = token.find_first_of("eE");
  const auto point = token.find('.');
  if (point == std::string_view::npos && exponent != std::string_view::npos) {
    output.append(token.substr(0, exponent));
    output.append(".0");
    output.append(token.substr(exponent));
    return;
  }
  output.append(token == "-0" ? std::string_view{"-0.0"} : token);
}

// The input as the oracle should see it. Only number tokens outside strings
// change, and only for an input both parsers accept, so the token boundaries
// found here are real ones.
[[nodiscard]] std::string oracle_input(const std::string_view input) {
  std::string output{};
  output.reserve(input.size() + 16U);
  bool in_string = false;
  for (std::size_t index = 0; index < input.size();) {
    const char character = input[index];
    if (in_string) {
      const auto length = character == '\\' ? std::size_t{2} : std::size_t{1};
      in_string = character != '"';
      output.append(input.substr(index, length));
      index += length;
      continue;
    }
    if (character == '-' || (character >= '0' && character <= '9')) {
      std::size_t end = index;
      while (end < input.size() && is_number_character(input[end])) {
        ++end;
      }
      append_oracle_number(output, input.substr(index, end - index));
      index = end;
      continue;
    }
    in_string = character == '"';
    output.push_back(character);
    ++index;
  }
  return output;
}

[[nodiscard]] scry::JsonKind old_kind(const JsonValue& value) {
  if (value.is_null()) {
    return scry::JsonKind::null;
  }
  if (value.is_boolean()) {
    return scry::JsonKind::boolean;
  }
  if (value.is_int64()) {
    return scry::JsonKind::signed_integer;
  }
  if (value.is_uint64()) {
    return scry::JsonKind::unsigned_integer;
  }
  if (value.is_double()) {
    return scry::JsonKind::number;
  }
  if (value.is_string()) {
    return scry::JsonKind::string;
  }
  return value.is_array() ? scry::JsonKind::array : scry::JsonKind::object;
}

void compare(const JsonValue& expected, const json::Value& actual,
             std::string_view input);

void compare_scalars(const JsonValue& expected, const json::Value& actual,
                     const std::string_view input) {
  bool same = true;
  switch (actual.kind()) {
  case scry::JsonKind::boolean:
    same = expected.get_boolean() == *actual.boolean();
    break;
  case scry::JsonKind::signed_integer:
    same = expected.get<std::int64_t>() == *actual.signed_integer();
    break;
  case scry::JsonKind::unsigned_integer:
    same = expected.get<std::uint64_t>() == *actual.unsigned_integer();
    break;
  case scry::JsonKind::number:
    same = std::bit_cast<std::uint64_t>(expected.get<double>()) ==
           std::bit_cast<std::uint64_t>(*actual.number());
    break;
  case scry::JsonKind::string:
    same = expected.get_string() == *actual.string();
    break;
  default:
    break;
  }
  if (!same) {
    fail("scalar values differ", input);
  }
}

void compare_containers(const JsonValue& expected, const json::Value& actual,
                        const std::string_view input) {
  if (const auto* elements = actual.array()) {
    const auto& expected_elements = expected.get_array();
    if (expected_elements.size() != elements->size()) {
      fail("array sizes differ", input);
    }
    for (std::size_t index = 0; index < elements->size(); ++index) {
      compare(expected_elements[index], (*elements)[index], input);
    }
    return;
  }
  const auto& expected_members = expected.get_object();
  const auto& members = *actual.object();
  if (expected_members.size() != members.size()) {
    fail("object sizes differ", input);
  }
  auto expected_member = expected_members.begin();
  for (const auto& member : members) {
    if (expected_member->first != member.first) {
      fail("object keys differ", input);
    }
    compare(expected_member->second, member.second, input);
    ++expected_member;
  }
}

void compare(const JsonValue& expected, const json::Value& actual,
             const std::string_view input) {
  if (old_kind(expected) != actual.kind()) {
    fail("node kinds differ", input);
  }
  if (actual.array() != nullptr || actual.object() != nullptr) {
    compare_containers(expected, actual, input);
  } else {
    compare_scalars(expected, actual, input);
  }
}

void check_quoting(const std::string_view input) {
  // The old codec's only string writer that takes arbitrary bytes.
  const auto old_quoted = scry::detail::make_json_error_object(input).text;
  const auto new_quoted = "{\"error\":" + json::quote(input) + "}";
  if (old_quoted != new_quoted) {
    fail("quoted strings differ", input);
  }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      const std::size_t size) {
  const auto input = std::string_view{reinterpret_cast<const char*>(data), size};
  check_quoting(input);

  const auto parsed = json::parse(input);
  if (parsed.has_value() != json::validate(input)) {
    fail("validate disagrees with parse", input);
  }
  const auto old_parsed = scry::detail::parse_json(
      input, scry::ErrorCategory::invalid_argument, "JSON text is not valid");
  if (old_parsed.has_value() != parsed.has_value()) {
    fail(parsed ? "only the new parser accepts" : "only the old parser accepts", input);
  }
  if (!parsed) {
    return 0;
  }

  const auto oracle_text = oracle_input(input);
  const auto oracle = scry::detail::parse_json(
      oracle_text, scry::ErrorCategory::invalid_argument, "JSON text is not valid");
  if (!oracle) {
    fail("the old parser rejects the rewritten input", input);
  }
  compare(*oracle, *parsed, input);

  const auto old_text =
      scry::detail::write_json_text(*oracle, scry::ErrorCategory::invalid_argument,
                                    "JSON value could not be written");
  if (!old_text || *old_text != json::write(*parsed)) {
    fail("canonical texts differ", input);
  }
  return 0;
}
