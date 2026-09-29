#pragma once

#include <cstdint>
#include <scry/annotations.hpp>
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
struct[[= scry::reflection::tag{"text"}]] TextBlock {
  /// UTF-8 text.
  std::string text{};
};

/// A model-issued tool call, which appears only in assistant messages.
struct[[= scry::reflection::tag{"tool_call"}]] ToolCallBlock {
  /// Provider-assigned call identifier, unique within the turn.
  std::string id{};
  /// Tool name requested by the model.
  std::string name{};
  /// Canonical JSON object passed to the tool handler.
  Json arguments{};
};

/// The result returned to the model for one tool call, which appears only in user
/// messages.
struct[[= scry::reflection::tag{"tool_result"}]] ToolResultBlock {
  /// Identifier of the ToolCallBlock this result answers.
  std::string tool_call_id{};
  /// Canonical JSON result sent back to the model.
  Json result{};
  /// Whether the result reports a tool failure rather than a value.
  bool is_error{false};
};

/// One piece of a message's content.
///
/// This is a reflected tagged variant: scry::reflection::encode() writes a block as
/// an object whose `"type"` member is its alternative's tag (`"text"`,
/// `"tool_call"`, or `"tool_result"`), and scry::reflection::decode() selects the
/// alternative by that member.
using ContentBlock = std::variant<TextBlock, ToolCallBlock, ToolResultBlock>;

/// One committed conversation message.
///
/// Messages are committed transactionally at a turn's successful terminal event, so a
/// message observed through Conversation::messages() is already part of the history
/// the next request will send.
///
/// A Message is a reflected value: a host can write one with
/// scry::reflection::encode() and read it back with scry::reflection::decode(). That
/// encoding is the per-message shape of the Conversation::to_json() document, with
/// `arguments` and `result` spliced as JSON values rather than quoted. Every member
/// has an initializer, so decode() reads an absent member as its initial value;
/// Conversation::from_json() also requires every member and checks the role and
/// block rules the codec does not express.
struct Message {
  /// Author of this message.
  Role role{Role::user};
  /// Ordered content blocks, as the provider dialect reported or Scry produced them.
  std::vector<ContentBlock> content{};
};

} // namespace scry
