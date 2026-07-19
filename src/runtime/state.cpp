#include "runtime/state.hpp"

namespace scry::detail {

std::string response_text(const ModelResponse& response) {
  std::string text;
  for (const auto& block : response.content) {
    if (const auto* content = std::get_if<TextBlock>(&block)) {
      text += content->text;
    }
  }
  return text;
}

} // namespace scry::detail
