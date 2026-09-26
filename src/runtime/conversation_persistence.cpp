#include "core/error.hpp"
#include "core/json_codec.hpp"
#include "runtime/state.hpp"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace scry {
namespace {

constexpr std::uint64_t conversation_document_version = 1;

[[nodiscard]] Error invalid_document(const std::string_view message) {
  return detail::make_error(ErrorCategory::invalid_config, std::string{message});
}

[[nodiscard]] Status
require_fields(const detail::JsonValue& value,
               const std::initializer_list<std::string_view> expected,
               const std::string_view context) {
  if (!value.is_object()) {
    return std::unexpected(
        invalid_document(std::string{context} + " must be an object"));
  }
  if (value.get_object().size() != expected.size()) {
    return std::unexpected(
        invalid_document(std::string{context} + " has missing or unknown fields"));
  }
  for (const auto name : expected) {
    if (!value.contains(name)) {
      return std::unexpected(
          invalid_document(std::string{context} + " has missing or unknown fields"));
    }
  }
  return {};
}

[[nodiscard]] Result<std::string_view> string_field(const detail::JsonValue& value,
                                                    const std::string_view name,
                                                    const std::string_view context) {
  const auto* found = detail::json_field(value, name);
  if (found == nullptr || !found->is_string()) {
    return std::unexpected(invalid_document(std::string{context} + " field '" +
                                            std::string{name} + "' must be a string"));
  }
  return found->get_string();
}

[[nodiscard]] Result<std::string_view>
nonempty_string_field(const detail::JsonValue& value, const std::string_view name,
                      const std::string_view context) {
  auto result = string_field(value, name, context);
  if (!result) {
    return std::unexpected(std::move(result.error()));
  }
  if (result->empty()) {
    return std::unexpected(invalid_document(std::string{context} + " field '" +
                                            std::string{name} + "' must not be empty"));
  }
  return result;
}

[[nodiscard]] Result<const detail::JsonValue::array_t*>
array_field(const detail::JsonValue& value, const std::string_view name,
            const std::string_view context) {
  const auto* found = detail::json_field(value, name);
  if (found == nullptr || !found->is_array()) {
    return std::unexpected(invalid_document(std::string{context} + " field '" +
                                            std::string{name} + "' must be an array"));
  }
  return &found->get_array();
}

[[nodiscard]] Result<bool> bool_field(const detail::JsonValue& value,
                                      const std::string_view name,
                                      const std::string_view context) {
  const auto* found = detail::json_field(value, name);
  if (found == nullptr || !found->is_boolean()) {
    return std::unexpected(invalid_document(std::string{context} + " field '" +
                                            std::string{name} + "' must be a boolean"));
  }
  return found->get_boolean();
}

[[nodiscard]] Result<detail::Role> decode_role(const detail::JsonValue& value) {
  auto role = string_field(value, "role", "Conversation message");
  if (!role) {
    return std::unexpected(std::move(role.error()));
  }
  if (*role == "user") {
    return detail::Role::user;
  }
  if (*role == "assistant") {
    return detail::Role::assistant;
  }
  return std::unexpected(invalid_document("Conversation message has an unknown role"));
}

[[nodiscard]] Result<detail::ContentBlock>
decode_text_block(const detail::JsonValue& value) {
  if (auto status = require_fields(value, {"text", "type"}, "Text block"); !status) {
    return std::unexpected(std::move(status.error()));
  }
  auto text = nonempty_string_field(value, "text", "Text block");
  if (!text) {
    return std::unexpected(std::move(text.error()));
  }
  return detail::TextBlock{.text = std::string{*text}};
}

[[nodiscard]] Result<detail::ContentBlock>
decode_tool_call(const detail::JsonValue& value, const detail::Role role) {
  if (role != detail::Role::assistant) {
    return std::unexpected(
        invalid_document("Tool-call blocks require the assistant role"));
  }
  if (auto status =
          require_fields(value, {"arguments", "id", "name", "type"}, "Tool-call block");
      !status) {
    return std::unexpected(std::move(status.error()));
  }
  auto id = nonempty_string_field(value, "id", "Tool-call block");
  if (!id) {
    return std::unexpected(std::move(id.error()));
  }
  auto name = nonempty_string_field(value, "name", "Tool-call block");
  if (!name) {
    return std::unexpected(std::move(name.error()));
  }
  // require_fields has already established that every field is present.
  const auto& arguments = *detail::json_field(value, "arguments");
  if (!arguments.is_object()) {
    return std::unexpected(
        invalid_document("Tool-call block field 'arguments' must be an object"));
  }
  auto encoded = detail::write_json(arguments, ErrorCategory::invalid_config,
                                    "Tool-call arguments could not be encoded");
  if (!encoded) {
    return std::unexpected(std::move(encoded.error()));
  }
  return detail::ToolCallBlock{
      .id = std::string{*id},
      .name = std::string{*name},
      .arguments = std::move(*encoded),
  };
}

[[nodiscard]] Result<detail::ContentBlock>
decode_tool_result(const detail::JsonValue& value, const detail::Role role) {
  if (role != detail::Role::user) {
    return std::unexpected(
        invalid_document("Tool-result blocks require the user role"));
  }
  if (auto status = require_fields(
          value, {"is_error", "result", "tool_call_id", "type"}, "Tool-result block");
      !status) {
    return std::unexpected(std::move(status.error()));
  }
  auto id = nonempty_string_field(value, "tool_call_id", "Tool-result block");
  if (!id) {
    return std::unexpected(std::move(id.error()));
  }
  auto is_error = bool_field(value, "is_error", "Tool-result block");
  if (!is_error) {
    return std::unexpected(std::move(is_error.error()));
  }
  // require_fields has already established that every field is present.
  auto encoded = detail::write_json(*detail::json_field(value, "result"),
                                    ErrorCategory::invalid_config,
                                    "Tool result could not be encoded");
  if (!encoded) {
    return std::unexpected(std::move(encoded.error()));
  }
  return detail::ToolResultBlock{
      .tool_call_id = std::string{*id},
      .result = std::move(*encoded),
      .is_error = *is_error,
  };
}

[[nodiscard]] Result<detail::ContentBlock> decode_block(const detail::JsonValue& value,
                                                        const detail::Role role) {
  if (!value.is_object()) {
    return std::unexpected(
        invalid_document("Conversation content block must be an object"));
  }
  auto type = string_field(value, "type", "Conversation content block");
  if (!type) {
    return std::unexpected(std::move(type.error()));
  }
  if (*type == "text") {
    return decode_text_block(value);
  }
  if (*type == "tool_call") {
    return decode_tool_call(value, role);
  }
  if (*type == "tool_result") {
    return decode_tool_result(value, role);
  }
  return std::unexpected(
      invalid_document("Conversation content block has an unknown type"));
}

[[nodiscard]] Result<detail::Message> decode_message(const detail::JsonValue& value) {
  if (auto status = require_fields(value, {"content", "role"}, "Conversation message");
      !status) {
    return std::unexpected(std::move(status.error()));
  }
  auto role = decode_role(value);
  if (!role) {
    return std::unexpected(std::move(role.error()));
  }
  auto content = array_field(value, "content", "Conversation message");
  if (!content) {
    return std::unexpected(std::move(content.error()));
  }
  if ((*content)->empty()) {
    return std::unexpected(
        invalid_document("Conversation message content must not be empty"));
  }
  detail::Message message{.role = *role};
  message.content.reserve((*content)->size());
  for (const auto& value_block : **content) {
    auto block = decode_block(value_block, *role);
    if (!block) {
      return std::unexpected(std::move(block.error()));
    }
    message.content.push_back(std::move(*block));
  }
  return message;
}

[[nodiscard]] Result<std::vector<detail::Message>>
decode_messages(const detail::JsonValue& root) {
  auto values = array_field(root, "messages", "Conversation document");
  if (!values) {
    return std::unexpected(std::move(values.error()));
  }
  std::vector<detail::Message> messages;
  messages.reserve((*values)->size());
  for (const auto& value : **values) {
    auto message = decode_message(value);
    if (!message) {
      return std::unexpected(std::move(message.error()));
    }
    messages.push_back(std::move(*message));
  }
  return messages;
}

[[nodiscard]] Result<detail::JsonValue>
parse_boundary_json(const Json& json, const bool require_object,
                    const std::string_view message) {
  auto value = detail::parse_json(json.text, ErrorCategory::invalid_config, message);
  if (!value) {
    return std::unexpected(std::move(value.error()));
  }
  if (require_object && !value->is_object()) {
    return std::unexpected(invalid_document(std::string{message}));
  }
  return value;
}

[[nodiscard]] Result<detail::JsonValue>
encode_block_value(const detail::TextBlock& block, detail::Role) {
  if (block.text.empty()) {
    return std::unexpected(invalid_document("Text block must not be empty"));
  }
  detail::JsonValue encoded{};
  encoded["text"] = block.text;
  encoded["type"] = "text";
  return encoded;
}

[[nodiscard]] Result<detail::JsonValue>
encode_block_value(const detail::ToolCallBlock& block, const detail::Role role) {
  if (role != detail::Role::assistant || block.id.empty() || block.name.empty()) {
    return std::unexpected(invalid_document("Invalid tool-call block"));
  }
  auto arguments = parse_boundary_json(block.arguments, true,
                                       "Tool-call arguments must be a JSON object");
  if (!arguments) {
    return std::unexpected(std::move(arguments.error()));
  }
  detail::JsonValue encoded{};
  encoded["arguments"] = std::move(*arguments);
  encoded["id"] = block.id;
  encoded["name"] = block.name;
  encoded["type"] = "tool_call";
  return encoded;
}

[[nodiscard]] Result<detail::JsonValue>
encode_block_value(const detail::ToolResultBlock& block, const detail::Role role) {
  if (role != detail::Role::user || block.tool_call_id.empty()) {
    return std::unexpected(invalid_document("Invalid tool-result block"));
  }
  auto result =
      parse_boundary_json(block.result, false, "Tool result must be valid JSON");
  if (!result) {
    return std::unexpected(std::move(result.error()));
  }
  detail::JsonValue encoded{};
  encoded["is_error"] = block.is_error;
  encoded["result"] = std::move(*result);
  encoded["tool_call_id"] = block.tool_call_id;
  encoded["type"] = "tool_result";
  return encoded;
}

[[nodiscard]] Result<detail::JsonValue> encode_block(const detail::ContentBlock& block,
                                                     const detail::Role role) {
  return std::visit(
      [role](const auto& value) { return encode_block_value(value, role); }, block);
}

[[nodiscard]] Result<detail::JsonValue> encode_message(const detail::Message& message) {
  if (message.content.empty()) {
    return std::unexpected(
        invalid_document("Conversation message content must not be empty"));
  }
  detail::JsonValue::array_t content;
  content.reserve(message.content.size());
  for (const auto& block : message.content) {
    auto encoded = encode_block(block, message.role);
    if (!encoded) {
      return std::unexpected(std::move(encoded.error()));
    }
    content.push_back(std::move(*encoded));
  }
  detail::JsonValue value{};
  detail::JsonValue content_value{};
  content_value.data = std::move(content);
  value["content"] = std::move(content_value);
  value["role"] = message.role == detail::Role::user ? "user" : "assistant";
  return value;
}

[[nodiscard]] Result<detail::JsonValue::array_t>
encode_messages(const std::vector<detail::Message>& messages) {
  detail::JsonValue::array_t encoded;
  encoded.reserve(messages.size());
  for (const auto& message : messages) {
    auto value = encode_message(message);
    if (!value) {
      return std::unexpected(std::move(value.error()));
    }
    encoded.push_back(std::move(*value));
  }
  return encoded;
}

[[nodiscard]] Result<std::uint64_t> decode_version(const detail::JsonValue& root) {
  const auto* version = detail::json_field(root, "version");
  if (version == nullptr || !version->is_uint64()) {
    return std::unexpected(
        invalid_document("Conversation document version must be an unsigned integer"));
  }
  return version->get<std::uint64_t>();
}

} // namespace

Result<Json> Conversation::to_json() const {
  if (impl_ == nullptr) {
    return std::unexpected(
        detail::make_error(ErrorCategory::invalid_state, "Conversation is inactive"));
  }
  auto messages = encode_messages(*impl_->messages);
  if (!messages) {
    return std::unexpected(std::move(messages.error()));
  }
  detail::JsonValue root{};
  detail::JsonValue message_value{};
  message_value.data = std::move(*messages);
  root["messages"] = std::move(message_value);
  root["system_prompt"] = impl_->config.system_prompt;
  root["version"] = conversation_document_version;
  return detail::write_json(root, ErrorCategory::invalid_config,
                            "Conversation document could not be encoded");
}

Result<Conversation> Conversation::from_json(const Json& json) {
  auto root = detail::parse_json(json.text, ErrorCategory::invalid_config,
                                 "Conversation document is not valid JSON");
  if (!root) {
    return std::unexpected(std::move(root.error()));
  }
  if (auto status = require_fields(*root, {"messages", "system_prompt", "version"},
                                   "Conversation document");
      !status) {
    return std::unexpected(std::move(status.error()));
  }
  auto version = decode_version(*root);
  if (!version) {
    return std::unexpected(std::move(version.error()));
  }
  if (*version != conversation_document_version) {
    return std::unexpected(
        invalid_document("Conversation document version is not supported"));
  }
  auto prompt = string_field(*root, "system_prompt", "Conversation document");
  if (!prompt) {
    return std::unexpected(std::move(prompt.error()));
  }
  auto messages = decode_messages(*root);
  if (!messages) {
    return std::unexpected(std::move(messages.error()));
  }
  auto impl = std::make_shared<Impl>();
  impl->config.system_prompt = std::string{*prompt};
  // Every counted byte is resident in the parsed document, so the sum cannot
  // overflow; saturation only keeps the arithmetic obviously total.
  impl->payload_bytes = impl->config.system_prompt.size();
  for (const auto& message : *messages) {
    impl->payload_bytes = detail::saturating_payload_add(
        impl->payload_bytes, detail::message_payload_bytes(message));
  }
  *impl->messages = std::move(*messages);
  return Conversation{std::move(impl)};
}

} // namespace scry
