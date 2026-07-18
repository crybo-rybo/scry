#include "chat_panel.hpp"

#include <imgui.h>
#include <memory>
#include <string>
#include <utility>

namespace {

class PassiveController final : public scry_showcase::PanelController {
public:
  [[nodiscard]] scry_showcase::SubmitStatus
  submit(std::string, scry_showcase::PanelCallbacks callbacks) override {
    callbacks_ = std::move(callbacks);
    return {};
  }

  [[nodiscard]] bool cancel() noexcept override { return false; }

private:
  scry_showcase::PanelCallbacks callbacks_{};
};

} // namespace

int main() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  auto& io = ImGui::GetIO();
  io.DisplaySize = ImVec2{640.0F, 480.0F};
  io.DeltaTime = 1.0F / 60.0F;
  unsigned char* pixels{};
  int atlas_width{};
  int atlas_height{};
  io.Fonts->GetTexDataAsRGBA32(&pixels, &atlas_width, &atlas_height);
  if (pixels == nullptr || atlas_width <= 0 || atlas_height <= 0) {
    ImGui::DestroyContext();
    return 1;
  }

  {
    PassiveController controller;
    scry_showcase::ChatPanel panel{controller};
    ImGui::NewFrame();
    panel.draw();
    ImGui::Render();
  }

  ImGui::DestroyContext();
  return 0;
}
