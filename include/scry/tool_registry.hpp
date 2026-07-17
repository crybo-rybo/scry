#pragma once

#include <cstddef>
#include <memory>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <scry/unique_function.hpp>
#include <string>

namespace scry {

struct ToolDefinition {
  std::string name{};
  std::string description{};
  Json input_schema{};
};

using ToolHandler = UniqueFunction<Result<Json>(Json)>;

class ToolRegistry final {
public:
  ~ToolRegistry();
  ToolRegistry(ToolRegistry&&) noexcept;
  ToolRegistry& operator=(ToolRegistry&&) noexcept;
  ToolRegistry(const ToolRegistry&) = delete;
  ToolRegistry& operator=(const ToolRegistry&) = delete;

  [[nodiscard]] Status add(ToolDefinition definition, ToolHandler handler);
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool empty() const noexcept;

private:
  class Impl;

  explicit ToolRegistry(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;

  friend class Harness;
};

} // namespace scry
