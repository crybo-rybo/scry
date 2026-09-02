#pragma once

#include <cstddef>
#include <scry/events.hpp>
#include <string_view>

namespace scry_showcase::npc {

[[nodiscard]] inline std::string_view
completion_failure(const scry::Completion& completion,
                   const std::size_t tool_call_count) noexcept {
  if (completion.finish_reason != scry::FinishReason::completed) {
    return "Model response did not complete normally";
  }
  if (tool_call_count == 0) {
    return "Model completed without executing an NPC tool";
  }
  if (completion.text.empty()) {
    return "Model completed without a final answer";
  }
  return {};
}

} // namespace scry_showcase::npc
