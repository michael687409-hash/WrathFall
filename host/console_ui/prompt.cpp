// PromptDialog implementation based on ReXGlue 0.11.0, BSD 3-Clause.
// The pinned ReXGlue license accompanies the SDK source under .tools.
#include "ui/prompt.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <string_view>

#include <imgui.h>
#include <rex/logging/log_category.h>
#include <rex/string/text.h>

#include "prompt_controller.h"

namespace rex::ui {
namespace {

constexpr ImVec4 kAccent(0.67f, 0.92f, 0.66f, 1.0f);
constexpr ImVec4 kSurface(0.12f, 0.15f, 0.17f, 1.0f);
constexpr ImVec4 kButton(0.20f, 0.24f, 0.26f, 1.0f);
constexpr ImVec4 kMuted(0.66f, 0.72f, 0.73f, 1.0f);

ImFont* GetPromptFont(ImFontAtlas& atlas) {
  constexpr char kFontName[] = "Asura prompt";
  for (ImFont* font : atlas.Fonts) {
    if (std::string_view(font->GetDebugName()) == kFontName) return font;
  }
  if (atlas.Locked) return nullptr;
  const char* face = "C:\\Windows\\Fonts\\segoeui.ttf";
  if (!std::filesystem::exists(face)) return nullptr;
  ImFontConfig font_config;
  string::CopyTruncating(font_config.Name, kFontName, sizeof(font_config.Name));
  ImFont* font = atlas.AddFontFromFileTTF(face, 17.0f, &font_config);
  const char* japanese_face = "C:\\Windows\\Fonts\\msgothic.ttc";
  if (font && std::filesystem::exists(japanese_face)) {
    ImFontConfig fallback;
    fallback.MergeMode = true;
    fallback.DstFont = font;
    atlas.AddFontFromFileTTF(japanese_face, 17.0f, &fallback);
  }
  return font;
}

}  // namespace

PromptDialog::PromptDialog(PromptConfig config, PromptDone done)
    : config_(std::move(config)), done_(std::move(done)) {
  if (config_.title.empty()) {
    if (config_.text_input && !config_.body.empty()) {
      config_.title = std::move(config_.body);
      config_.body.clear();
    } else {
      config_.title = config_.text_input ? "Keyboard Input" : "Xbox 360";
    }
  }
  if (config_.text_input) {
    buttons_ = {"OK", "Cancel"};
    buffer_.assign((config_.max_length != 0 ? config_.max_length : 1024) + 1, '\0');
    string::CopyTruncating(buffer_.data(), config_.default_text, buffer_.size());
  } else {
    buttons_ = config_.buttons.empty() ? std::vector<std::string>{"OK"} : config_.buttons;
  }
  label_ = std::format("{}###prompt{}", config_.title, reinterpret_cast<u64>(this));
}

PromptDialog::~PromptDialog() { Complete({}); }

void PromptDialog::Complete(PromptResult result) { done_(std::move(result)); }

void PromptDialog::Choose(u32 button, std::string text) {
  REXUI_DEBUG("Console prompt selected button {}", button);
  Complete(PromptResult{.canceled = false, .button = button, .text = std::move(text)});
  closing_ = true;
}

std::string PromptDialog::GetText() const { return std::string(buffer_.data()); }

void PromptDialog::DrawButtons() {
  constexpr int kSelected = 1;
  constexpr int kPreviousButtons = 2;
  constexpr int kOpenedAt = 3;
  ImGuiStorage& storage = *ImGui::GetStateStorage();
  const int count = static_cast<int>(buttons_.size());
  int selected = storage.GetInt(kSelected, 0);
  const unsigned current = asura::prompt::ReadControllers();
  const int previous_buttons = storage.GetInt(kPreviousButtons, -1);
  const unsigned pressed = asura::prompt::JustPressed(current, previous_buttons);
  storage.SetInt(kPreviousButtons, static_cast<int>(current));
  const float now = static_cast<float>(ImGui::GetTime());
  if (previous_buttons < 0) storage.SetFloat(kOpenedAt, now);

  // Prevent the input that brought up the prompt from answering it.
  if (now - storage.GetFloat(kOpenedAt) >= 0.18f && ImGui::IsWindowFocused()) {
    const bool previous = (pressed & asura::prompt::kPrevious) ||
                          ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) ||
                          ImGui::IsKeyPressed(ImGuiKey_UpArrow, false);
    const bool next = (pressed & asura::prompt::kNext) ||
                      ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) ||
                      ImGui::IsKeyPressed(ImGuiKey_DownArrow, false);
    selected = (selected + count + static_cast<int>(next) - static_cast<int>(previous)) % count;
    storage.SetInt(kSelected, selected);

    if ((pressed & asura::prompt::kBack) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      REXUI_DEBUG("Console prompt received Back");
      ImGui::CloseCurrentPopup();
      if (buttons_.back() == "No" || buttons_.back() == "Cancel") {
        Choose(static_cast<u32>(count - 1), {});
      } else {
        Complete({});
        closing_ = true;
      }
      return;
    }
    if ((pressed & asura::prompt::kConfirm) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
      ImGui::CloseCurrentPopup();
      Choose(static_cast<u32>(selected), {});
      return;
    }
  }

  const bool stack = count > 3;
  const float width = ImGui::GetContentRegionAvail().x;
  const float gap = ImGui::GetStyle().ItemSpacing.x;
  const float button_width = stack ? width : (width - (count - 1) * gap) / count;
  for (int index = 0; index < count; ++index) {
    if (!stack && index != 0) ImGui::SameLine();
    const bool focused = index == selected;
    ImGui::PushStyleColor(ImGuiCol_Button, focused ? kAccent : kButton);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, focused ? kAccent : ImVec4(0.30f, 0.35f, 0.36f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, focused ? kAccent : kButton);
    if (focused) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.09f, 0.15f, 0.10f, 1.0f));
    ImGui::PushID(index);
    const bool picked = ImGui::Button(buttons_[index].c_str(), ImVec2(button_width, 42.0f));
    ImGui::PopID();
    ImGui::PopStyleColor(focused ? 4 : 3);
    if (picked) {
      ImGui::CloseCurrentPopup();
      Choose(static_cast<u32>(index), {});
      return;
    }
  }
  ImGui::Spacing();
  ImGui::TextColored(kMuted, "D-pad  Select     A  Confirm     B  Back");
}

void PromptDialog::DrawInput() {
  ImGui::SetNextItemWidth(-1.0f);
  const bool entered = ImGui::InputText("##body", buffer_.data(), buffer_.size(),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
  const bool accepted = ImGui::Button("OK") || entered;
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) {
    ImGui::CloseCurrentPopup();
    Complete({});
    closing_ = true;
    return;
  }
  if (accepted) {
    ImGui::CloseCurrentPopup();
    Choose(0, GetText());
  }
}

bool PromptDialog::Draw(ImGuiIO& io) {
  const bool first_draw = !opened_;
  if (first_draw) {
    ImGui::OpenPopup(label_.c_str());
    opened_ = true;
  }
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  const float width = std::min(std::clamp(viewport->WorkSize.x * 0.58f, 400.0f, 630.0f),
                               std::max(1.0f, viewport->WorkSize.x - 30.0f));
  ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f, 19.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 9.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 11.0f));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, kSurface);
  ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.02f, 0.03f, 0.04f, 0.60f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.33f, 0.43f, 0.40f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.96f, 0.96f, 1.0f));

  const bool visible = ImGui::BeginPopupModal(label_.c_str(), nullptr, ImGuiWindowFlags_NoTitleBar |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
      (config_.text_input ? 0 : ImGuiWindowFlags_NoNavInputs));
  if (visible) {
    ImFont* font = GetPromptFont(*io.Fonts);
    ImGui::PushFont(font, 22.0f);
    ImGui::TextUnformatted(config_.title.c_str());
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::PushFont(font, 17.0f);
    if (!config_.body.empty()) ImGui::TextWrapped("%s", config_.body.c_str());
    if (config_.text_input) {
      if (first_draw) ImGui::SetKeyboardFocusHere();
      DrawInput();
    } else {
      if (first_draw) ImGui::GetStateStorage()->Clear();
      DrawButtons();
    }
    ImGui::PopFont();
    ImGui::EndPopup();
  } else if (!ImGui::IsPopupOpen(label_.c_str())) {
    REXUI_DEBUG("Console prompt closed externally");
    Complete({});
    closing_ = true;
  }
  ImGui::PopStyleColor(4);
  ImGui::PopStyleVar(4);
  return !closing_;
}

}  // namespace rex::ui
