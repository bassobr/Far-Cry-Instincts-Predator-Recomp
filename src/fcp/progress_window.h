// Progress window of the first start: building the game code and the shader pack.
//
// The window lives on its own thread and the build never waits on it: when it cannot be made (no display, a
// window thread stuck in the window system) the build runs to the end all the same. Closing it asks whether to
// stop; Cancelled() then turns true and the build ends its processes. FCP_TEST_PROGRESS_WINDOW=fail|hang (tests):
// the window cannot be made / its thread blocks for good.
//
// Adapted from src/app/game_code.cpp of Far Cry Instincts Recomp (MIT, same author), which adapted it from
// SCDA-XBERecomp's src/app/game_code.cpp (MIT License, Copyright (c) TGP482 and Joshhhuaaa).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace fcp::progress {

enum class Stage : int { kDisc, kCodegen, kCompile, kLink, kShaders, kDone };

/** Opens the window (on its own thread). */
void Begin();

/** Sets the stage and its line of text (the bar follows the stage and the counts). */
void SetStage(Stage stage, const std::wstring& text);

/** Compile progress: bytes of source compiled of the total, for the bar and the time left. */
void SetCompile(uint64_t done_bytes, uint64_t total_bytes, uint32_t done_files, uint32_t total_files,
                uint32_t first_wave);

/** Fraction 0..1 within the current stage (disc, codegen, shaders). */
void SetStageFraction(float fraction);

/** True once the player chose to stop. */
bool Cancelled();

/** Stops the build as the player's "Yes" would (FCP_TEST_GAME_CODE_CANCEL_AFTER, tests). */
void RequestCancel();

/** Asks the window to close; logs how it went. */
void End(bool ok);

/** "", ", without the progress window" or ", the progress window not up yet" for log lines. */
const char* WindowNote();

/** Error box: the message, the end of the build log and where the full log is. */
void ShowFailure(const std::wstring& message, const std::filesystem::path& build_log);

/** The last ~1500 bytes of a log, from a whole line. */
std::wstring LogTail(const std::filesystem::path& log);

}  // namespace fcp::progress
