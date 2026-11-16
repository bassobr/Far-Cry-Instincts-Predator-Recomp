/**
 * @file        ui/prompt.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <concepts>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <rex/types.h>
#include <rex/ui/ui_system.h>
#include <rex/ui/dialog.h>
#include <rex/utility.h>

namespace rex::ui {

class PromptDone : public rex::MoveOnly {
 public:
  PromptDone() = default;

  template <typename F>
    requires(!std::same_as<std::remove_cvref_t<F>, PromptDone> && std::invocable<F&, PromptResult>)
  PromptDone(F&& done) : done_(std::forward<F>(done)) {}

  PromptDone(PromptDone&& other) noexcept : done_(std::move(other.done_)) { other.done_ = nullptr; }

  PromptDone& operator=(PromptDone&& other) noexcept {
    if (this != &other) {
      (*this)(PromptResult{});
      done_ = std::move(other.done_);
      other.done_ = nullptr;
    }
    return *this;
  }

  ~PromptDone() { (*this)(PromptResult{}); }

  void operator()(PromptResult result) {
    if (!done_) {
      return;
    }
    auto done = std::move(done_);
    done_ = nullptr;
    done(std::move(result));
  }

  explicit operator bool() const { return static_cast<bool>(done_); }

 private:
  rex::MoveOnlyFunction<void(PromptResult)> done_;
};

class PromptDialog final : public DialogBase {
 public:
  PromptDialog(PromptConfig config, PromptDone done);
  ~PromptDialog() override;

  void Choose(u32 button, std::string text);
  std::string GetText() const;

  bool Draw(ImGuiIO& io) override;

  // FCP (UPSTREAM 381): the on-screen keyboard of a text prompt (pad, mouse, touch). The physical keyboard types
  // into the text field as before. Off with FCP_KEYBOARD_UI=0 (the plain field of upstream).
  /** Keys of the on-screen keyboard: kRows rows of kColumns characters, then one row of kActions actions. */
  static constexpr int kRows = 4;
  static constexpr int kColumns = 10;
  enum class Action : u8 { kShift, kSpace, kDelete, kOk, kCancel };
  static constexpr int kActions = 5;
  /** Appends text at the end while it fits (whole UTF-8 characters). */
  void Insert(std::string_view text);
  /** Removes the last character (one UTF-8 character). */
  void DeleteLast();
  /** The character of a key, after shift. */
  char KeyChar(int row, int column) const;
  /** Presses the selected key (row kRows = the action row). */
  void PressSelected();
  void Press(Action action);
  /** Moves the selection, wrapping around. */
  void MoveSelection(int dx, int dy);
  int SelectedRow() const { return sel_row_; }
  int SelectedColumn() const { return sel_col_; }
  bool Shift() const { return shift_; }
  bool Closing() const { return closing_; }

 private:
  void Complete(PromptResult result);
  void DrawButtons();
  void DrawInput();
  void DrawKeyboardInput(ImGuiIO& io);
  void HandlePad();

  PromptConfig config_;
  PromptDone done_;
  std::vector<std::string> buttons_;
  std::vector<char> buffer_;
  std::string label_;
  bool opened_ = false;
  bool closing_ = false;
  bool keyboard_ui_ = true;
  bool shift_ = false;
  bool reload_ = false;  // the text changed outside the field (or the field got the focus again): cursor to the end
  int sel_row_ = 1;
  int sel_col_ = 0;
};

}  // namespace rex::ui
