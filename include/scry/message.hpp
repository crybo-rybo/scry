#pragma once

#include <cstdint>
#include <scry/json.hpp>
#include <string>
#include <variant>
#include <vector>

namespace scry {

/// Author of a committed message.
enum class Role : std::uint8_t {
  /// The host application, including tool results returned to the model.
  user,
  /// The model.
  assistant,
};

/// Plain text content.
struct TextBlock {
  /// UTF-8 text.
  std::string text{};
};

/// A model-issued tool call, which appears only in assistant messages.
struct ToolCallBlock {
  /// Provider-assigned call identifier, unique within the turn.
  std::string id{};
  /// Registered tool name the model asked for.
  std::string name{};
  /// Canonical JSON object passed to the tool handler.
  Json arguments{};
};

/// The result returned to the model for one tool call, which appears only in user
/// messages.
struct ToolResultBlock {
  /// Identifier of the ToolCallBlock this result answers.
  std::string tool_call_id{};
  /// Canonical JSON result sent back to the model.
  Json result{};
  /// Whether the result reports a tool failure rather than a value.
  bool is_error{false};
};

/// One piece of a message's content.
using ContentBlock = std::variant<TextBlock, ToolCallBlock, ToolResultBlock>;

/// One committed conversation message.
///
/// Messages are committed transactionally at a turn's successful terminal event, so a
/// message observed through Conversation::messages() is already part of the history
/// the next request will send.
struct Message {
  /// Author of this message.
  Role role{Role::user};
  /// Ordered content blocks, as the provider dialect reported or Scry produced them.
  std::vector<ContentBlock> content{};
};

} // namespace scry
