#include "ui/prompt.h"

#include <algorithm>
#include <cstdlib>
#include <format>

#include <imgui.h>
#include <imgui_internal.h>

#include <rex/string/text.h>

namespace rex::ui {

namespace {

constexpr u32 kDefaultTextCapacity = 1024;

// FCP (UPSTREAM 381): the on-screen keyboard, ordered like the console's (letters A..Z, digits on top)
constexpr const char* kKeyRows[PromptDialog::kRows] = {"1234567890", "abcdefghij", "klmnopqrst", "uvwxyz-_.'"};
constexpr const char* kActionLabels[PromptDialog::kActions] = {"Shift", "Space", "Del", "OK", "Cancel"};

bool KeyboardUiEnabled() {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  const char* v = std::getenv("FCP_KEYBOARD_UI");  // read only
#pragma clang diagnostic pop
  return !(v && v[0] == '0');
}

}  // namespace

PromptDialog::PromptDialog(PromptConfig config, PromptDone done)
    : config_(std::move(config)), done_(std::move(done)) {
  if (config_.title.empty()) {
    if (config_.text_input && !config_.body.empty()) {
      config_.title = std::move(config_.body);
      config_.body.clear();
    } else {
      config_.title = config_.text_input ? "Keyboard Input" : "Message Box";
    }
  }
  if (config_.text_input) {
    buttons_ = {"OK", "Cancel"};
    buffer_.assign((config_.max_length != 0 ? config_.max_length : kDefaultTextCapacity) + 1, '\0');
    string::CopyTruncating(buffer_.data(), config_.default_text, buffer_.size());
    keyboard_ui_ = KeyboardUiEnabled();
  } else {
    buttons_ = config_.buttons.empty() ? std::vector<std::string>{"OK"} : config_.buttons;
  }
  label_ = std::format("{}###prompt{}", config_.title, reinterpret_cast<u64>(this));
}

PromptDialog::~PromptDialog() {
  Complete({});
}

void PromptDialog::Complete(PromptResult result) {
  done_(std::move(result));
}

void PromptDialog::Choose(u32 button, std::string text) {
  Complete(PromptResult{.canceled = false, .button = button, .text = std::move(text)});
  closing_ = true;
}

std::string PromptDialog::GetText() const {
  return std::string(buffer_.data());
}

void PromptDialog::Insert(std::string_view text) {
  if (buffer_.empty()) {
    return;
  }
  size_t length = std::char_traits<char>::length(buffer_.data());
  for (size_t at = 0; at < text.size();) {
    size_t n = 1;  // one whole UTF-8 character
    const auto lead = static_cast<unsigned char>(text[at]);
    if (lead >= 0xF0) {
      n = 4;
    } else if (lead >= 0xE0) {
      n = 3;
    } else if (lead >= 0xC0) {
      n = 2;
    }
    n = std::min(n, text.size() - at);
    if (length + n + 1 > buffer_.size()) {
      break;
    }
    std::copy_n(text.data() + at, n, buffer_.data() + length);
    length += n;
    buffer_[length] = '\0';
    at += n;
  }
  reload_ = true;
}

void PromptDialog::DeleteLast() {
  size_t length = std::char_traits<char>::length(buffer_.data());
  while (length > 0) {
    --length;
    const auto c = static_cast<unsigned char>(buffer_[length]);
    buffer_[length] = '\0';
    if ((c & 0xC0) != 0x80) {
      break;
    }
  }
  reload_ = true;
}

char PromptDialog::KeyChar(int row, int column) const {
  const char c = kKeyRows[row][column];
  return shift_ && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

void PromptDialog::Press(Action action) {
  switch (action) {
    case Action::kShift:
      shift_ = !shift_;
      break;
    case Action::kSpace:
      Insert(" ");
      break;
    case Action::kDelete:
      DeleteLast();
      break;
    case Action::kOk:
      if (!closing_) {
        Choose(0, GetText());
      }
      break;
    case Action::kCancel:
      if (!closing_) {
        Complete({});
        closing_ = true;
      }
      break;
  }
}

void PromptDialog::PressSelected() {
  if (sel_row_ == kRows) {
    Press(static_cast<Action>(sel_col_ * kActions / kColumns));
    return;
  }
  const char c = KeyChar(sel_row_, sel_col_);
  Insert(std::string_view(&c, 1));
}

void PromptDialog::MoveSelection(int dx, int dy) {
  sel_row_ = (sel_row_ + dy + kRows + 1) % (kRows + 1);
  if (sel_row_ == kRows) {
    // the action row has kActions keys of kColumns / kActions columns each: a step moves by one key
    constexpr int span = kColumns / kActions;
    const int key = ((sel_col_ / span) + dx + kActions) % kActions;
    sel_col_ = key * span;
  } else {
    sel_col_ = (sel_col_ + dx + kColumns) % kColumns;
  }
}

void PromptDialog::DrawButtons() {
  for (size_t index = 0; index < buttons_.size(); ++index) {
    if (index != 0) {
      ImGui::SameLine();
    }
    if (ImGui::Button(std::format("{}##{}", buttons_[index], index).c_str())) {
      ImGui::CloseCurrentPopup();
      Choose(static_cast<u32>(index), {});
      return;
    }
  }
}

void PromptDialog::DrawInput() {
  const bool entered = ImGui::InputText("##body", buffer_.data(), buffer_.size(),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
  bool accepted = entered;
  if (ImGui::Button("OK")) {
    accepted = true;
  }
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

// The pad as ImGui's gamepad keys (fed by the application while a prompt is open), like the console's keyboard:
// D-pad or left stick move, A presses the key, X deletes, Y types a space, LB/LT shift, Start accepts, B cancels.
void PromptDialog::HandlePad() {
  auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, true); };
  auto once = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, false); };
  if (pressed(ImGuiKey_GamepadDpadLeft) || pressed(ImGuiKey_GamepadLStickLeft)) MoveSelection(-1, 0);
  if (pressed(ImGuiKey_GamepadDpadRight) || pressed(ImGuiKey_GamepadLStickRight)) MoveSelection(1, 0);
  if (pressed(ImGuiKey_GamepadDpadUp) || pressed(ImGuiKey_GamepadLStickUp)) MoveSelection(0, -1);
  if (pressed(ImGuiKey_GamepadDpadDown) || pressed(ImGuiKey_GamepadLStickDown)) MoveSelection(0, 1);
  if (pressed(ImGuiKey_GamepadFaceDown)) PressSelected();
  if (pressed(ImGuiKey_GamepadFaceLeft)) Press(Action::kDelete);
  if (pressed(ImGuiKey_GamepadFaceUp)) Press(Action::kSpace);
  if (once(ImGuiKey_GamepadL1) || once(ImGuiKey_GamepadL2)) Press(Action::kShift);
  if (once(ImGuiKey_GamepadStart)) Press(Action::kOk);
  if (once(ImGuiKey_GamepadFaceRight)) Press(Action::kCancel);
}

void PromptDialog::DrawKeyboardInput(ImGuiIO& io) {
  (void)io;
  // Escape cancels (the field owns the keyboard keys while it is active, so ask as its owner too)
  const ImGuiID field_id = ImGui::GetID("##body");
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
      ImGui::IsKeyPressed(ImGuiKey_Escape, ImGuiInputFlags_None, field_id)) {
    Press(Action::kCancel);
  }
  if (!closing_) {
    HandlePad();
  }
  if (closing_) {
    ImGui::CloseCurrentPopup();
    return;
  }

  const ImGuiStyle& style = ImGui::GetStyle();
  const float key = ImGui::GetFontSize() * 1.9f;
  const float width = key * kColumns + style.ItemSpacing.x * (kColumns - 1);

  // the text field keeps the keyboard focus while nothing else is held, so the physical keyboard always types
  if (!ImGui::IsAnyItemActive()) {
    ImGui::SetKeyboardFocusHere();
    reload_ = true;
  }
  if (reload_) {
    if (ImGuiInputTextState* state = ImGui::GetInputTextState(field_id)) {
      state->ReloadUserBufAndMoveToEnd();
      reload_ = false;
    }
  }
  ImGui::SetNextItemWidth(width);
  const bool entered = ImGui::InputText("##body", buffer_.data(), buffer_.size(),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
  if (entered) {
    Press(Action::kOk);
    ImGui::CloseCurrentPopup();
    return;
  }
  ImGui::Spacing();

  const ImVec4 selected = style.Colors[ImGuiCol_ButtonActive];
  for (int row = 0; row <= kRows; ++row) {
    const bool actions = row == kRows;
    const int count = actions ? kActions : kColumns;
    const float w = actions ? (width - style.ItemSpacing.x * (kActions - 1)) / kActions : key;
    for (int column = 0; column < count; ++column) {
      if (column != 0) {
        ImGui::SameLine();
      }
      const int sel_column = actions ? column * (kColumns / kActions) : column;
      const bool is_selected = row == sel_row_ && sel_column == sel_col_;
      const bool shift_on = actions && static_cast<Action>(column) == Action::kShift && shift_;
      if (is_selected || shift_on) {
        ImGui::PushStyleColor(ImGuiCol_Button, is_selected ? selected : style.Colors[ImGuiCol_ButtonHovered]);
      }
      std::string label;
      if (actions) {
        label = std::format("{}##key{}_{}", kActionLabels[column], row, column);
      } else {
        label = std::format("{}##key{}_{}", KeyChar(row, column), row, column);
      }
      const bool clicked = ImGui::Button(label.c_str(), ImVec2(w, key * 0.9f));
      if (is_selected || shift_on) {
        ImGui::PopStyleColor();
      }
      if (clicked) {
        sel_row_ = row;
        sel_col_ = sel_column;
        PressSelected();
      }
    }
  }
  ImGui::Spacing();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);  // the hint must not widen the window
  ImGui::TextDisabled("Pad: A key, X delete, Y space, LB shift, Start OK, B cancel");
  ImGui::PopTextWrapPos();
  if (closing_) {
    ImGui::CloseCurrentPopup();
  }
}

bool PromptDialog::Draw(ImGuiIO& io) {
  bool first_draw = false;
  if (!opened_) {
    ImGui::OpenPopup(label_.c_str());
    opened_ = true;
    first_draw = true;
  }
  const bool keyboard = config_.text_input && keyboard_ui_;
  if (keyboard) {
    // readable at every window size and inside the window (about 23 x 15 font heights), centred
    const float fit = std::min({io.DisplaySize.y / 30.0f, io.DisplaySize.y * 0.9f / 15.0f,
                                io.DisplaySize.x * 0.9f / 23.0f});
    ImGui::PushFont(nullptr, std::clamp(fit, 11.0f, 44.0f));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
  }
  if (!ImGui::BeginPopupModal(label_.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (keyboard) {
      ImGui::PopFont();
    }
    if (!closing_) {
      Complete({});
    }
    closing_ = true;
    return false;
  }
  if (!config_.body.empty()) {
    ImGui::TextWrapped("%s", config_.body.c_str());
  }
  if (keyboard) {
    DrawKeyboardInput(io);
  } else {
    if (first_draw) {
      ImGui::SetKeyboardFocusHere();
    }
    if (config_.text_input) {
      DrawInput();
    } else {
      DrawButtons();
    }
  }
  ImGui::Spacing();
  ImGui::EndPopup();
  if (keyboard) {
    ImGui::PopFont();
  }
  return !closing_;
}

}  // namespace rex::ui
