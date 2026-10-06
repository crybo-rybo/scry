#include "kernel/kernel.hpp"

#include "kernel/json/document.hpp"

#include "kernel/json/number.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace scry::detail::json {

// The parser's only way to give a Value content.
class Builder {
public:
  template <class T> [[nodiscard]] static Value make(T data) {
    return Value{Value::Data{std::in_place_type<T>, std::move(data)}};
  }
};

namespace {

// Stands in for the value under construction when the reader only validates.
struct Discard {};

[[nodiscard]] bool is_whitespace(const char character) noexcept {
  return character == ' ' || character == '\t' || character == '\n' ||
         character == '\r';
}

[[nodiscard]] bool is_digit(const char character) noexcept {
  return character >= '0' && character <= '9';
}

// The value of one hexadecimal digit, or -1.
[[nodiscard]] int hex_value(const char character) noexcept {
  if (is_digit(character)) {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return character - 'a' + 10;
  }
  if (character >= 'A' && character <= 'F') {
    return character - 'A' + 10;
  }
  return -1;
}

// String bodies are scanned eight bytes at a time until a word holds a byte
// that needs a closer look. The byte-wise tests are the exact "has less than"
// and "has zero" word tricks, valid for bounds up to 0x80.
constexpr std::size_t word_size = sizeof(std::uint64_t);
constexpr std::uint64_t byte_ones = 0x0101010101010101U;
constexpr std::uint64_t byte_highs = 0x8080808080808080U;

[[nodiscard]] std::uint64_t load_word(const char* bytes) noexcept {
  std::uint64_t word = 0;
  std::memcpy(&word, bytes, word_size);
  return word;
}

[[nodiscard]] constexpr std::uint64_t bytes_below(const std::uint64_t word,
                                                  const std::uint64_t bound) noexcept {
  return (word - byte_ones * bound) & ~word & byte_highs;
}

[[nodiscard]] constexpr std::uint64_t bytes_equal(const std::uint64_t word,
                                                  const unsigned char byte) noexcept {
  return bytes_below(word ^ (byte_ones * byte), 1U);
}

// A control character, a quote, or a backslash: what quoting must escape.
[[nodiscard]] constexpr bool word_needs_escape(const std::uint64_t word) noexcept {
  return (bytes_below(word, 0x20U) | bytes_equal(word, '"') |
          bytes_equal(word, '\\')) != 0;
}

// Eight bytes a string body may hold as they are: ASCII needing no escape.
[[nodiscard]] constexpr bool word_is_plain(const std::uint64_t word) noexcept {
  return (word & byte_highs) == 0 && !word_needs_escape(word);
}

// A UTF-8 lead byte's sequence length and the range its second byte must fall
// in (Unicode Table 3-7, "Well-Formed UTF-8 Byte Sequences"). Length zero marks
// a byte that cannot start a sequence: a continuation byte, an overlong lead
// (C0, C1), or one beyond U+10FFFF (F5 and up).
struct Utf8Lead {
  std::size_t length{};
  unsigned int low{};
  unsigned int high{};
};

[[nodiscard]] Utf8Lead utf8_lead(const unsigned char byte) noexcept {
  if (byte < 0xC2U) {
    return {};
  }
  if (byte <= 0xDFU) {
    return {.length = 2, .low = 0x80U, .high = 0xBFU};
  }
  if (byte <= 0xEFU) {
    // E0 would be overlong below A0; ED above 9F encodes a surrogate.
    return {.length = 3,
            .low = byte == 0xE0U ? 0xA0U : 0x80U,
            .high = byte == 0xEDU ? 0x9FU : 0xBFU};
  }
  if (byte <= 0xF4U) {
    // F0 would be overlong below 90; F4 above 8F is past U+10FFFF.
    return {.length = 4,
            .low = byte == 0xF0U ? 0x90U : 0x80U,
            .high = byte == 0xF4U ? 0x8FU : 0xBFU};
  }
  return {};
}

void append_utf8(std::string& output, const std::uint32_t code_point) {
  const auto byte = [](const std::uint32_t value) { return static_cast<char>(value); };
  if (code_point < 0x80U) {
    output.push_back(byte(code_point));
  } else if (code_point < 0x800U) {
    output.push_back(byte(0xC0U | (code_point >> 6U)));
    output.push_back(byte(0x80U | (code_point & 0x3FU)));
  } else if (code_point < 0x10000U) {
    output.push_back(byte(0xE0U | (code_point >> 12U)));
    output.push_back(byte(0x80U | ((code_point >> 6U) & 0x3FU)));
    output.push_back(byte(0x80U | (code_point & 0x3FU)));
  } else {
    output.push_back(byte(0xF0U | (code_point >> 18U)));
    output.push_back(byte(0x80U | ((code_point >> 12U) & 0x3FU)));
    output.push_back(byte(0x80U | ((code_point >> 6U) & 0x3FU)));
    output.push_back(byte(0x80U | (code_point & 0x3FU)));
  }
}

[[nodiscard]] char simple_escape(const char escape) noexcept {
  switch (escape) {
  case '"':
  case '\\':
  case '/':
    return escape;
  case 'b':
    return '\b';
  case 'f':
    return '\f';
  case 'n':
    return '\n';
  case 'r':
    return '\r';
  case 't':
    return '\t';
  default:
    return '\0';
  }
}

// Sorts an object's members by key and keeps the last of each run of equal
// keys. The stable sort leaves duplicates in document order, so the last of a
// run is the last occurrence.
void canonicalize_members(Object& members) {
  const auto out_of_order = [](const Member& left, const Member& right) {
    return !(left.first < right.first);
  };
  if (std::ranges::adjacent_find(members, out_of_order) == members.end()) {
    return;
  }
  std::ranges::stable_sort(members, [](const Member& left, const Member& right) {
    return left.first < right.first;
  });
  auto kept = members.begin();
  for (auto member = members.begin(); member != members.end(); ++member) {
    const auto next = std::next(member);
    if (next != members.end() && next->first == member->first) {
      continue;
    }
    if (kept != member) {
      *kept = std::move(*member);
    }
    ++kept;
  }
  members.erase(kept, members.end());
}

// One recursive-descent reader for both the parser and the validator, so the
// two cannot disagree about the grammar. Build selects whether it produces a
// Value; without it the reader touches no heap memory.
template <bool Build> class Reader {
public:
  using Out = std::conditional_t<Build, Value, Discard>;
  using Text = std::conditional_t<Build, std::string, Discard>;
  using Elements = std::conditional_t<Build, Array, Discard>;
  using Members = std::conditional_t<Build, Object, Discard>;

  explicit Reader(const std::string_view text) noexcept
      : cursor_(text.data()), end_(text.data() + text.size()) {}

  [[nodiscard]] bool document(Out& out) {
    if (!value(out)) {
      return false;
    }
    skip_whitespace();
    return cursor_ == end_;
  }

private:
  [[nodiscard]] bool at_end() const noexcept { return cursor_ == end_; }

  [[nodiscard]] bool next_is(const char character) const noexcept {
    return cursor_ != end_ && *cursor_ == character;
  }

  void skip_whitespace() noexcept {
    while (cursor_ != end_ && is_whitespace(*cursor_)) {
      ++cursor_;
    }
  }

  template <class T> static void emit(Out& out, T&& data) {
    if constexpr (Build) {
      out = Builder::make<std::remove_cvref_t<T>>(std::forward<T>(data));
    }
  }

  [[nodiscard]] bool value(Out& out) {
    skip_whitespace();
    if (at_end()) {
      return false;
    }
    switch (*cursor_) {
    case '{':
      return object(out);
    case '[':
      return array(out);
    case '"':
      return string_value(out);
    case 't':
      return literal("true", out, true);
    case 'f':
      return literal("false", out, false);
    case 'n':
      return literal("null", out, nullptr);
    default:
      return number(out);
    }
  }

  template <class T>
  [[nodiscard]] bool literal(const std::string_view word, Out& out, T data) {
    if (static_cast<std::size_t>(end_ - cursor_) < word.size() ||
        std::memcmp(cursor_, word.data(), word.size()) != 0) {
      return false;
    }
    cursor_ += word.size();
    emit(out, data);
    return true;
  }

  [[nodiscard]] bool enter() noexcept {
    ++cursor_;
    return ++depth_ <= max_depth;
  }

  // After an element or member: a comma continues the container and the
  // closing character ends it.
  [[nodiscard]] bool separator(const char close, bool& closed) noexcept {
    skip_whitespace();
    if (at_end()) {
      return false;
    }
    const char character = *cursor_++;
    closed = character == close;
    return closed || character == ',';
  }

  [[nodiscard]] bool array(Out& out) {
    if (!enter()) {
      return false;
    }
    Elements elements{};
    skip_whitespace();
    bool closed = next_is(']');
    if (closed) {
      ++cursor_;
    }
    while (!closed) {
      Out element{};
      if (!value(element) || !separator(']', closed)) {
        return false;
      }
      if constexpr (Build) {
        elements.push_back(std::move(element));
      }
    }
    --depth_;
    emit(out, std::move(elements));
    return true;
  }

  [[nodiscard]] bool member(Members& members) {
    skip_whitespace();
    Text key{};
    if (!next_is('"') || !read_string(key)) {
      return false;
    }
    skip_whitespace();
    if (!next_is(':')) {
      return false;
    }
    ++cursor_;
    Out element{};
    if (!value(element)) {
      return false;
    }
    if constexpr (Build) {
      members.emplace_back(std::move(key), std::move(element));
    }
    return true;
  }

  [[nodiscard]] bool object(Out& out) {
    if (!enter()) {
      return false;
    }
    Members members{};
    skip_whitespace();
    bool closed = next_is('}');
    if (closed) {
      ++cursor_;
    }
    while (!closed) {
      if (!member(members) || !separator('}', closed)) {
        return false;
      }
    }
    --depth_;
    if constexpr (Build) {
      canonicalize_members(members);
    }
    emit(out, std::move(members));
    return true;
  }

  [[nodiscard]] bool string_value(Out& out) {
    Text text{};
    if (!read_string(text)) {
      return false;
    }
    emit(out, std::move(text));
    return true;
  }

  // Reads a string token, the cursor on its opening quote. Runs of bytes that
  // need no decoding are copied whole.
  [[nodiscard]] bool read_string(Text& text) {
    ++cursor_;
    const char* run = cursor_;
    const auto flush = [&] {
      if constexpr (Build) {
        text.append(run, cursor_);
      }
    };
    while (cursor_ != end_) {
      if (static_cast<std::size_t>(end_ - cursor_) >= word_size &&
          word_is_plain(load_word(cursor_))) {
        cursor_ += word_size;
        continue;
      }
      const auto byte = static_cast<unsigned char>(*cursor_);
      if (byte == '"') {
        flush();
        ++cursor_;
        return true;
      }
      if (byte == '\\') {
        flush();
        ++cursor_;
        if (!escape(text)) {
          return false;
        }
        run = cursor_;
        continue;
      }
      if (byte < 0x20U) {
        return false;
      }
      if (byte < 0x80U) {
        ++cursor_;
      } else if (!utf8_sequence(byte)) {
        return false;
      }
    }
    return false;
  }

  [[nodiscard]] bool utf8_sequence(const unsigned char lead) noexcept {
    const auto expected = utf8_lead(lead);
    if (expected.length == 0 ||
        static_cast<std::size_t>(end_ - cursor_) < expected.length) {
      return false;
    }
    const auto second = static_cast<unsigned char>(cursor_[1]);
    if (second < expected.low || second > expected.high) {
      return false;
    }
    for (std::size_t index = 2; index < expected.length; ++index) {
      const auto continuation = static_cast<unsigned char>(cursor_[index]);
      if (continuation < 0x80U || continuation > 0xBFU) {
        return false;
      }
    }
    cursor_ += expected.length;
    return true;
  }

  // The escape after a backslash, the cursor past the backslash.
  [[nodiscard]] bool escape(Text& text) {
    if (at_end()) {
      return false;
    }
    const char escaped = *cursor_++;
    if (escaped == 'u') {
      return unicode_escape(text);
    }
    const char decoded = simple_escape(escaped);
    if (decoded == '\0') {
      return false;
    }
    if constexpr (Build) {
      text.push_back(decoded);
    }
    return true;
  }

  // Four hex digits, the cursor on the first; empty when malformed.
  [[nodiscard]] std::optional<std::uint32_t> hex_quad() noexcept {
    if (end_ - cursor_ < 4) {
      return std::nullopt;
    }
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
      const int digit = hex_value(*cursor_++);
      if (digit < 0) {
        return std::nullopt;
      }
      value = (value << 4U) | static_cast<std::uint32_t>(digit);
    }
    return value;
  }

  [[nodiscard]] bool unicode_escape(Text& text) {
    auto code_point = hex_quad();
    if (!code_point || (*code_point >= 0xDC00U && *code_point <= 0xDFFFU)) {
      return false;
    }
    if (*code_point >= 0xD800U && *code_point <= 0xDBFFU) {
      if (end_ - cursor_ < 2 || cursor_[0] != '\\' || cursor_[1] != 'u') {
        return false;
      }
      cursor_ += 2;
      const auto low = hex_quad();
      if (!low || *low < 0xDC00U || *low > 0xDFFFU) {
        return false;
      }
      code_point = 0x10000U + ((*code_point - 0xD800U) << 10U) + (*low - 0xDC00U);
    }
    if constexpr (Build) {
      append_utf8(text, *code_point);
    }
    return true;
  }

  // One or more digits; false when there is none.
  [[nodiscard]] bool digits() noexcept {
    const char* start = cursor_;
    while (cursor_ != end_ && is_digit(*cursor_)) {
      ++cursor_;
    }
    return cursor_ != start;
  }

  [[nodiscard]] bool exponent() noexcept {
    ++cursor_;
    if (next_is('+') || next_is('-')) {
      ++cursor_;
    }
    return digits();
  }

  // Scans the RFC 8259 number grammar; true when well formed. `plain` ends false
  // when the number has a fraction or an exponent.
  [[nodiscard]] bool scan_number(bool& plain) noexcept {
    if (next_is('-')) {
      ++cursor_;
    }
    if (next_is('0')) {
      ++cursor_;
    } else if (at_end() || *cursor_ < '1' || *cursor_ > '9' || !digits()) {
      return false;
    }
    if (next_is('.')) {
      ++cursor_;
      if (!digits()) {
        return false;
      }
      plain = false;
    }
    if (next_is('e') || next_is('E')) {
      plain = false;
      return exponent();
    }
    return true;
  }

  [[nodiscard]] bool number(Out& out) {
    const char* start = cursor_;
    bool plain = true;
    if (!scan_number(plain)) {
      return false;
    }
    const std::string_view token{start, cursor_};
    if (plain && integer(token, out)) {
      return true;
    }
    const auto converted = parse_double(start, cursor_);
    if (!converted) {
      return false;
    }
    emit(out, *converted);
    return true;
  }

  // A plain integer token that fits its kind; false sends it to the double path.
  [[nodiscard]] static bool integer(std::string_view token, Out& out) {
    const bool negative = token.front() == '-';
    if (negative) {
      token.remove_prefix(1);
    }
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t magnitude = 0;
    for (const char digit : token) {
      const auto value = static_cast<std::uint64_t>(digit - '0');
      if (magnitude > (max - value) / 10U) {
        return false;
      }
      magnitude = magnitude * 10U + value;
    }
    if (!negative) {
      emit(out, magnitude);
      return true;
    }
    constexpr auto min_magnitude = std::uint64_t{1} << 63U;
    if (magnitude == 0 || magnitude > min_magnitude) {
      return false;
    }
    // Two's complement negation; 2^63 becomes the minimum int64.
    emit(out, static_cast<std::int64_t>(~magnitude + 1U));
    return true;
  }

  const char* cursor_{};
  const char* end_{};
  std::size_t depth_{};
};

// ---------------------------------------------------------------------------
// The canonical writer.
// ---------------------------------------------------------------------------

template <class Integer> void append_integer(std::string& output, const Integer value) {
  std::array<char, 24> buffer{};
  const auto written =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  output.append(buffer.data(), written.ptr);
}

[[nodiscard]] const char* short_escape(const unsigned char byte) noexcept {
  switch (byte) {
  case '"':
    return "\\\"";
  case '\\':
    return "\\\\";
  case '\b':
    return "\\b";
  case '\f':
    return "\\f";
  case '\n':
    return "\\n";
  case '\r':
    return "\\r";
  case '\t':
    return "\\t";
  default:
    return nullptr;
  }
}

[[nodiscard]] bool needs_escape(const char character) noexcept {
  const auto byte = static_cast<unsigned char>(character);
  return byte < 0x20U || byte == '"' || byte == '\\';
}

void append_escape(std::string& output, const unsigned char byte) {
  if (const char* escape = short_escape(byte)) {
    output.append(escape);
    return;
  }
  constexpr std::string_view hexadecimal = "0123456789ABCDEF";
  output.append("\\u00");
  output.push_back(hexadecimal[byte >> 4U]);
  output.push_back(hexadecimal[byte & 0x0FU]);
}

void write_value(const Value& value, std::string& output);

void write_array(const Array& elements, std::string& output) {
  output.push_back('[');
  for (std::size_t index = 0; index < elements.size(); ++index) {
    if (index > 0) {
      output.push_back(',');
    }
    write_value(elements[index], output);
  }
  output.push_back(']');
}

void write_object(const Object& members, std::string& output) {
  output.push_back('{');
  for (std::size_t index = 0; index < members.size(); ++index) {
    if (index > 0) {
      output.push_back(',');
    }
    append_quoted(output, members[index].first);
    output.push_back(':');
    write_value(members[index].second, output);
  }
  output.push_back('}');
}

void write_scalar(const Value& value, std::string& output) {
  // Integers before number(), which reports them too.
  if (const auto flag = value.boolean()) {
    output.append(*flag ? "true" : "false");
  } else if (const auto negative = value.signed_integer()) {
    append_integer(output, *negative);
  } else if (const auto natural = value.unsigned_integer()) {
    append_integer(output, *natural);
  } else if (const auto number = value.number()) {
    append_double(output, *number);
  } else if (const auto text = value.string()) {
    append_quoted(output, *text);
  } else {
    output.append("null");
  }
}

void write_value(const Value& value, std::string& output) {
  if (const auto* elements = value.array()) {
    write_array(*elements, output);
  } else if (const auto* members = value.object()) {
    write_object(*members, output);
  } else {
    write_scalar(value, output);
  }
}

} // namespace

JsonKind Value::kind() const noexcept { return static_cast<JsonKind>(data_.index()); }

bool Value::is_null() const noexcept {
  return std::holds_alternative<std::nullptr_t>(data_);
}

std::optional<bool> Value::boolean() const noexcept {
  const auto* value = std::get_if<bool>(&data_);
  return value == nullptr ? std::nullopt : std::optional<bool>{*value};
}

std::optional<std::int64_t> Value::signed_integer() const noexcept {
  const auto* value = std::get_if<std::int64_t>(&data_);
  return value == nullptr ? std::nullopt : std::optional<std::int64_t>{*value};
}

std::optional<std::uint64_t> Value::unsigned_integer() const noexcept {
  const auto* value = std::get_if<std::uint64_t>(&data_);
  return value == nullptr ? std::nullopt : std::optional<std::uint64_t>{*value};
}

std::optional<double> Value::number() const noexcept {
  if (const auto* value = std::get_if<double>(&data_)) {
    return *value;
  }
  if (const auto* value = std::get_if<std::uint64_t>(&data_)) {
    return static_cast<double>(*value);
  }
  if (const auto* value = std::get_if<std::int64_t>(&data_)) {
    return static_cast<double>(*value);
  }
  return std::nullopt;
}

std::optional<std::string_view> Value::string() const noexcept {
  const auto* value = std::get_if<std::string>(&data_);
  return value == nullptr ? std::nullopt : std::optional<std::string_view>{*value};
}

const Array* Value::array() const noexcept { return std::get_if<Array>(&data_); }

const Object* Value::object() const noexcept { return std::get_if<Object>(&data_); }

std::size_t Value::size() const noexcept {
  if (const auto* elements = array()) {
    return elements->size();
  }
  if (const auto* members = object()) {
    return members->size();
  }
  return 0;
}

const Value* Value::find(const std::string_view key) const noexcept {
  const auto* members = object();
  if (members == nullptr) {
    return nullptr;
  }
  const auto found =
      std::lower_bound(members->begin(), members->end(), key,
                       [](const Member& member, const std::string_view name) {
                         return member.first < name;
                       });
  if (found == members->end() || found->first != key) {
    return nullptr;
  }
  return &found->second;
}

std::optional<Value> parse(const std::string_view text) {
  Value value{};
  Reader<true> reader{text};
  if (!reader.document(value)) {
    return std::nullopt;
  }
  return value;
}

bool validate(const std::string_view text) noexcept {
  Discard discarded{};
  Reader<false> reader{text};
  return reader.document(discarded);
}

void write(const Value& value, std::string& output) { write_value(value, output); }

std::string write(const Value& value) {
  std::string output{};
  write_value(value, output);
  return output;
}

void append_quoted(std::string& output, const std::string_view text) {
  output.push_back('"');
  const auto* run = text.data();
  const auto* const end = text.data() + text.size();
  for (const auto* cursor = run; cursor != end;) {
    if (static_cast<std::size_t>(end - cursor) >= word_size &&
        !word_needs_escape(load_word(cursor))) {
      cursor += word_size;
      continue;
    }
    if (needs_escape(*cursor)) {
      output.append(run, cursor);
      append_escape(output, static_cast<unsigned char>(*cursor));
      run = cursor + 1;
    }
    ++cursor;
  }
  output.append(run, end);
  output.push_back('"');
}

std::string quote(const std::string_view text) {
  std::string output{};
  output.reserve(text.size() + 2U);
  append_quoted(output, text);
  return output;
}

} // namespace scry::detail::json
