// Save games: a backup after every save, a check at the start that puts a damaged save back to its last good
// state, and an end of the program that waits for a save in progress. PC comfort, the game saves as on the console.
// Model: FCI's save guard (Far Cry Instincts Recomp, src/kernel/k_save.cpp, FCI_SAVE_GUARD, MIT, same author); the
// files and the way of saving differ (docs/phase1/spielstand-schutz.md).
//
// How Predator saves (measured): a saved game is XContent of type 1 ("save:" root), a host folder
//   user\<xuid>\<title>\00000001\<name>\ (the game's files and __thumbnail.png) and its header
//   user\<xuid>\<title>\Headers\00000001\<name>.header (XCONTENT_AGGREGATE_DATA), written at the level start and at
//   every checkpoint with XamContentCreate(CREATE_ALWAYS) ... XamContentClose. The profile's title settings are
//   files below user\<title>\profile\ (written whole and renamed by the SDK).
// Atomic: the SDK writes a replaced package beside the old one and swaps them at the close (rexglue/UPSTREAM.md,
//   change 330, folders <name>.saving / .ready / .old).
// Backups: Root()\save_backup\<xuid>\<title>\<type>\<name>\<time>\ (content\, header, profile\), the newest
//   FCP_SAVE_BACKUPS (default 3) kept. Damaged files go to Root()\save_damaged\<time>\...; nothing is deleted.
//
// Switches: FCP_SAVE_GUARD=0 none of this (saves are still checked and logged; the SDK deletes and writes as
// upstream), FCP_SAVE_ATOMIC=0 only the SDK's replacement off, FCP_SAVE_BACKUPS=<n>, FCP_SAVE_EXIT_WAIT_MS (5000).
// Tests: FCP_TEST_SAVE_END=<n>:<kill|close|menu|restart>[:<ms>] ends the program <ms> after the n-th save of this
// process started (kill = TerminateProcess as a crash; close = WM_CLOSE to the window; menu = F10 menu open, then Exit
// Game; restart = the F10 menu's Apply restart);
// FCP_TEST_SAVE_END_GENS like FCP_INPUT_SCRIPT_GENS (default: generation 0 only).
#pragma once

#include <cstdint>
#include <string>

namespace fcp::save_guard {

/** At the start, after fcp::paths::Configure/Migrate and the log: checks every save, repairs, starts the backups. */
void Startup();

/** Import hooks (fcp/imports.cpp): XamContentCreate(Ex) before the call and its return, XamContentClose's return. */
void OnCreate(const std::string& root, uint32_t type, const std::string& file, uint32_t flags);
void OnCreateDone(uint32_t result);
void OnClose(const std::string& root, uint32_t result);

/** The program ends (window closed, Exit Game, the end of a relaunch): waits for a save in progress, at most
    FCP_SAVE_EXIT_WAIT_MS. */
void WaitForSave(const char* why);

}  // namespace fcp::save_guard
