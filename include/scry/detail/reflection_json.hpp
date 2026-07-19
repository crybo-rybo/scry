#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <string>
#include <string_view>

namespace scry::reflection::detail {

enum class JsonKind : std::uint8_t {
  null,
  boolean,
  signed_integer,
  unsigned_integer,
  number,
  string,
  array,
  object,
};

class JsonView final {
public:
  JsonView() = default;

  [[nodiscard]] JsonKind kind() const noexcept;
  [[nodiscard]] std::optional<bool> boolean() const noexcept;
  [[nodiscard]] std::optional<std::int64_t> signed_integer() const noexcept;
  [[nodiscard]] std::optional<std::uint64_t> unsigned_integer() const noexcept;
  [[nodiscard]] std::optional<double> number() const noexcept;
  [[nodiscard]] std::optional<std::string_view> string() const noexcept;

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::optional<JsonView> at(std::size_t index) const noexcept;
  [[nodiscard]] std::optional<std::string_view>
  key_at(std::size_t index) const noexcept;
  [[nodiscard]] std::optional<JsonView> find(std::string_view name) const noexcept;

private:
  class Document;

  JsonView(std::shared_ptr<const Document> document, const void* value) noexcept;

  std::shared_ptr<const Document> document_{};
  const void* value_{};

  friend Result<JsonView> parse_json(Json);
};

[[nodiscard]] Result<JsonView> parse_json(Json json);
void append_json_string(std::string& output, std::string_view value);

} // namespace scry::reflection::detail
