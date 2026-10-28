#pragma once

#include <expected>
#include <string>
#include <utility>

namespace scrydantic {

/// Error value of every fallible Scrydantic operation.
///
/// Scrydantic reports what failed and where, and leaves classification to the
/// caller, which knows whether the input came from a user, a model, or a peer.
struct Error {
  /// Human-readable diagnostic for the host.
  std::string message{};
  /// Text meant for a model, or empty when the failure carries nothing a model
  /// could act on. Scrydantic fills it for strict decode failures, whose every
  /// word is derived from a schema the model was already given; a tool handler
  /// fills it through tool_error().
  std::string model_message{};
};

/// Builds the error a tool handler returns to show the model why its call failed.
///
/// An empty `host_message` copies `model_message` into `Error::message`, so a
/// handler that wants one diagnostic writes it once.
/// @param model_message Text meant for the model.
/// @param host_message Host-side diagnostic kept in Error::message; empty copies
/// model_message.
/// @return An Error carrying both texts.
[[nodiscard]] inline Error tool_error(std::string model_message,
                                      std::string host_message = {}) {
  // A handler that supplies no host text still wants the refusal in its own logs,
  // and repeating text it already chose to show the model discloses nothing new.
  if (host_message.empty()) {
    host_message = model_message;
  }
  return Error{
      .message = std::move(host_message),
      .model_message = std::move(model_message),
  };
}

/// Result of a fallible Scrydantic operation.
template <typename T> using Result = std::expected<T, Error>;

/// Result of a fallible operation that returns no value on success.
using Status = Result<void>;

} // namespace scrydantic
