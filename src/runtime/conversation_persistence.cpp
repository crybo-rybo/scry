#include "kernel/error.hpp"
#include "kernel/json/codec.hpp"
#include "reflection/codec.hpp"
#include "runtime/state.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <meta>
#include <scry/annotations.hpp>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <scry/message.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace scry {
namespace {

constexpr std::uint64_t conversation_document_version = 1;

// The persisted document. Its members have no initializers, so decoding requires
// each of them; unknown members are rejected at every level.
struct ConversationDocument {
  std::vector<Message> messages;
  std::string system_prompt;
  std::uint64_t version;
};

// The same document borrowed from a Conversation for encoding, member for member.
struct ConversationDocumentView {
  const std::vector<Message>& messages;
  std::string_view system_prompt;
  std::uint64_t version;
};

// Read before the document itself, so a document from another version reports its
// version rather than whatever shape difference it has from this one.
struct[[= reflection::ignore_unknown]] ConversationDocumentHeader {
  std::uint64_t version;
};

[[nodiscard]] Error invalid_document(std::string message) {
  return detail::make_error(ErrorCategory::invalid_config, std::move(message));
}

[[nodiscard]] Error invalid_document(const detail::CodecFailure& failure) {
  return invalid_document("Conversation document at " + detail::describe(failure));
}

// ---- Completeness ------------------------------------------------------------

// The codec reads a member that has an initializer as optional, and every member
// of the public message model has one, so it accepts a message or block that
// omits one. Unknown members are already rejected, so this finds the declared
// members the document left out. `path` is relative to the document root, as a
// codec failure's is.
template <typename Type>
[[nodiscard]] Status require_members(const JsonView& object, const std::string& path) {
  template for (constexpr auto field : reflection::detail::fields_v<Type>) {
    constexpr std::string_view key = field.key_view();
    if (!object.find(key).has_value()) {
      return std::unexpected(invalid_document(detail::CodecFailure{
          .reason = "is a required member",
          .path = path + "." + std::string{key},
      }));
    }
  }
  return {};
}

[[nodiscard]] Status require_complete_message(const JsonView& value,
                                              const Message& message,
                                              const std::string& path) {
  if (auto status = require_members<Message>(value, path); !status) {
    return status;
  }
  const auto content = *value.find("content");
  for (std::size_t index = 0; index < message.content.size(); ++index) {
    const auto block_path = path + ".content[" + std::to_string(index) + "]";
    auto status = std::visit(
        [&]<typename Block>(const Block&) {
          return require_members<Block>(*content.at(index), block_path);
        },
        message.content[index]);
    if (!status) {
      return status;
    }
  }
  return {};
}

[[nodiscard]] Status require_complete(const JsonView& root,
                                      const std::vector<Message>& messages) {
  const auto values = *root.find("messages");
  for (std::size_t index = 0; index < messages.size(); ++index) {
    if (auto status =
            require_complete_message(*values.at(index), messages[index],
                                     ".messages[" + std::to_string(index) + "]");
        !status) {
      return status;
    }
  }
  return {};
}

// ---- Semantic rules ----------------------------------------------------------

// What the codec cannot express: which role may carry which block, nonempty
// text and identifiers, and object arguments. Both directions apply them, so a
// document to_json() writes is one from_json() accepts.
[[nodiscard]] Status validate_block(const TextBlock& block, Role) {
  if (block.text.empty()) {
    return std::unexpected(
        invalid_document("Text block field 'text' must not be empty"));
  }
  return {};
}

[[nodiscard]] Status validate_block(const ToolCallBlock& block, const Role role) {
  if (role != Role::assistant) {
    return std::unexpected(
        invalid_document("Tool-call blocks require the assistant role"));
  }
  if (block.id.empty()) {
    return std::unexpected(
        invalid_document("Tool-call block field 'id' must not be empty"));
  }
  if (block.name.empty()) {
    return std::unexpected(
        invalid_document("Tool-call block field 'name' must not be empty"));
  }
  // Decoded arguments are canonical text, and encoding validates the stored text
  // as it splices it, so the first byte is all that is left to check.
  if (!detail::json_root_is_object(block.arguments.text)) {
    return std::unexpected(
        invalid_document("Tool-call block field 'arguments' must be an object"));
  }
  return {};
}

[[nodiscard]] Status validate_block(const ToolResultBlock& block, const Role role) {
  if (role != Role::user) {
    return std::unexpected(
        invalid_document("Tool-result blocks require the user role"));
  }
  if (block.tool_call_id.empty()) {
    return std::unexpected(
        invalid_document("Tool-result block field 'tool_call_id' must not be empty"));
  }
  return {};
}

[[nodiscard]] Status validate_messages(const std::vector<Message>& messages) {
  for (const auto& message : messages) {
    if (message.content.empty()) {
      return std::unexpected(
          invalid_document("Conversation message content must not be empty"));
    }
    for (const auto& block : message.content) {
      auto status = std::visit(
          [&message](const auto& value) { return validate_block(value, message.role); },
          block);
      if (!status) {
        return status;
      }
    }
  }
  return {};
}

} // namespace

Result<Json> Conversation::to_json() const {
  if (impl_ == nullptr) {
    return std::unexpected(
        detail::make_error(ErrorCategory::invalid_state, "Conversation is inactive"));
  }
  if (auto status = validate_messages(*impl_->messages); !status) {
    return std::unexpected(std::move(status.error()));
  }
  auto encoded = detail::encode_text(ConversationDocumentView{
      .messages = *impl_->messages,
      .system_prompt = impl_->config.system_prompt,
      .version = conversation_document_version,
  });
  if (!encoded) {
    return std::unexpected(invalid_document(encoded.error()));
  }
  // The splice keeps stored arguments and results byte for byte; this pass is what
  // makes the document canonical.
  return detail::canonicalize_json(Json{.text = std::move(*encoded)},
                                   ErrorCategory::invalid_config,
                                   "Conversation document could not be encoded");
}

Result<Conversation> Conversation::from_json(const Json& json) {
  auto root = JsonView::parse(json);
  if (!root) {
    return std::unexpected(invalid_document("Conversation document is not valid JSON"));
  }
  auto header = detail::decode_value<ConversationDocumentHeader>(*root);
  if (!header) {
    return std::unexpected(invalid_document(header.error()));
  }
  if (header->version != conversation_document_version) {
    return std::unexpected(
        invalid_document("Conversation document version is not supported"));
  }
  auto document = detail::decode_value<ConversationDocument>(*root);
  if (!document) {
    return std::unexpected(invalid_document(document.error()));
  }
  if (auto status = require_complete(*root, document->messages); !status) {
    return std::unexpected(std::move(status.error()));
  }
  if (auto status = validate_messages(document->messages); !status) {
    return std::unexpected(std::move(status.error()));
  }

  auto impl = std::make_shared<Impl>();
  impl->config.system_prompt = std::move(document->system_prompt);
  // Every counted byte is resident in the parsed document, so the sum cannot
  // overflow; saturation only keeps the arithmetic obviously total.
  impl->payload_bytes = impl->config.system_prompt.size();
  for (const auto& message : document->messages) {
    impl->payload_bytes = detail::saturating_payload_add(
        impl->payload_bytes, detail::message_payload_bytes(message));
  }
  *impl->messages = std::move(document->messages);
  return Conversation{std::move(impl)};
}

} // namespace scry
