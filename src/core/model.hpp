#pragma once

#include <cstdint>
#include <scry/config.hpp>
#include <scry/json.hpp>
#include <string>
#include <variant>
#include <vector>

namespace scry::detail {

enum class Role : std::uint8_t {
  user,
  assistant,
};

struct TextBlock {
  std::string text{};
};

struct ToolCallBlock {
  std::string id{};
  std::string name{};
  Json arguments{};
};

struct ToolResultBlock {
  std::string tool_call_id{};
  Json result{};
  bool is_error{false};
};

using ContentBlock = std::variant<TextBlock, ToolCallBlock, ToolResultBlock>;

struct Message {
  Role role{Role::user};
  std::vector<ContentBlock> content{};
};

struct ToolSchema {
  std::string name{};
  std::string description{};
  Json input_schema{};
};

struct ModelRequest {
  std::string model{};
  std::string system_prompt{};
  std::vector<Message> messages{};
  std::vector<ToolSchema> tools{};
  SamplingConfig sampling{};
  bool streaming{true};
};

enum class FinishReason : std::uint8_t {
  completed,
  length,
  tool_use,
  unknown,
};

struct Usage {
  std::uint64_t input_tokens{};
  std::uint64_t output_tokens{};
};

struct ModelResponse {
  std::vector<ContentBlock> content{};
  FinishReason finish_reason{FinishReason::unknown};
  Usage usage{};
  std::string provider_request_id{};
};

} // namespace scry::detail
