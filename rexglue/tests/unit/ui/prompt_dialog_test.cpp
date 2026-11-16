/**
 * @file        tests/unit/ui/prompt_dialog_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <memory>
#include <string>

#include <rex/ui/dialog.h>

#include "ui/prompt.h"

using rex::ui::PromptDialog;
using rex::ui::PromptDone;
using rex::ui::PromptResult;

namespace {

struct Answers {
  int calls = 0;
  PromptResult last;

  PromptDone Done() {
    return PromptDone([this](PromptResult result) {
      ++calls;
      last = std::move(result);
    });
  }
};

}  // namespace

REX_TEST_CASE("a prompt completes once with the pressed button", "[ui][prompt]") {
  Answers answers;
  {
    PromptDialog dialog({.title = "Pick", .buttons = {"No", "Yes"}}, answers.Done());
    dialog.Choose(1, {});
    CHECK(answers.calls == 1);
    CHECK_FALSE(answers.last.canceled);
    CHECK(answers.last.button == 1);
  }
  CHECK(answers.calls == 1);
}

REX_TEST_CASE("a text prompt returns the accepted text", "[ui][prompt]") {
  Answers answers;
  PromptDialog dialog({.title = "Name", .text_input = true, .default_text = "abc"}, answers.Done());
  CHECK(dialog.GetText() == "abc");
  dialog.Choose(0, dialog.GetText());
  CHECK(answers.calls == 1);
  CHECK(answers.last.text == "abc");
}

REX_TEST_CASE("a text prompt holds max_length characters", "[ui][prompt]") {
  Answers answers;
  PromptDialog dialog(
      {.title = "Name", .text_input = true, .default_text = "abcdef", .max_length = 3},
      answers.Done());
  CHECK(dialog.GetText() == "abc");
}

REX_TEST_CASE("a prompt destroyed without a button completes cancelled", "[ui][prompt]") {
  Answers answers;
  {
    PromptDialog dialog({.title = "Pick"}, answers.Done());
    CHECK(answers.calls == 0);
  }
  CHECK(answers.calls == 1);
  CHECK(answers.last.canceled);
}

// FCP (UPSTREAM 381): the on-screen keyboard of a text prompt
REX_TEST_CASE("the on-screen keyboard types, shifts and deletes", "[ui][prompt]") {
  Answers answers;
  PromptDialog dialog({.title = "Cheat", .text_input = true, .max_length = 18}, answers.Done());
  CHECK(dialog.SelectedRow() == 1);  // starts on the letter a
  CHECK(dialog.SelectedColumn() == 0);
  dialog.Press(PromptDialog::Action::kShift);
  dialog.MoveSelection(6, 0);  // g
  dialog.PressSelected();
  CHECK(dialog.GetText() == "G");
  dialog.Press(PromptDialog::Action::kShift);
  dialog.MoveSelection(2, 0);  // i
  dialog.PressSelected();
  dialog.Press(PromptDialog::Action::kSpace);
  CHECK(dialog.GetText() == "Gi ");
  dialog.Press(PromptDialog::Action::kDelete);
  CHECK(dialog.GetText() == "Gi");
  dialog.Press(PromptDialog::Action::kOk);
  CHECK(answers.calls == 1);
  CHECK_FALSE(answers.last.canceled);
  CHECK(answers.last.text == "Gi");
  CHECK(dialog.Closing());
}

REX_TEST_CASE("the on-screen keyboard wraps and reaches the action row", "[ui][prompt]") {
  Answers answers;
  PromptDialog dialog({.title = "Cheat", .text_input = true}, answers.Done());
  dialog.MoveSelection(-1, 0);  // from a to the last key of the row
  CHECK(dialog.SelectedColumn() == PromptDialog::kColumns - 1);
  dialog.MoveSelection(0, -2);  // over the digits to the action row
  CHECK(dialog.SelectedRow() == PromptDialog::kRows);
  dialog.MoveSelection(1, 0);  // the action row wraps by whole keys: Cancel -> Shift
  CHECK(dialog.SelectedColumn() == 0);
  dialog.MoveSelection(-1, 0);  // back to Cancel
  dialog.PressSelected();
  CHECK(answers.calls == 1);
  CHECK(answers.last.canceled);
}

REX_TEST_CASE("the on-screen keyboard keeps max_length and whole UTF-8 characters", "[ui][prompt]") {
  Answers answers;
  PromptDialog dialog({.title = "Name", .text_input = true, .default_text = "ab", .max_length = 3},
                      answers.Done());
  dialog.Insert("\xC3\xA4");  // a 2 byte character does not fit after "ab" in 3 bytes
  CHECK(dialog.GetText() == "ab");
  dialog.Insert("c");
  dialog.Insert("d");
  CHECK(dialog.GetText() == "abc");
  dialog.DeleteLast();
  dialog.DeleteLast();
  dialog.Insert("\xC3\xA4");
  CHECK(dialog.GetText() == "a\xC3\xA4");
  dialog.DeleteLast();
  CHECK(dialog.GetText() == "a");
}
