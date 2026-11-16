// Speech volume ([audio] speech_volume, FCP_SPEECH_VOLUME): the game's speech gets its own level, apart from music
// and effects. Speech of Far Cry Instincts Predator lives in the language sound banks (dvdsound.dat entries
// "sounds_dvd\<language>\<bank>.sb4"; the bank names come from the sound project "sounds_dvd\farcry360.sp4", which
// lists every bank). This module follows the game's reads of dvdsound.dat (fat, project, banks) and registers the
// bytes of every bank read as speech or other sound by content (rex/fcp_speech.h: the game copies its reads out of a
// staging buffer, so the address does not tell); the XMA decoder then scales the frames whose input is registered
// speech. The game itself mixes and plays as before.
// Idea from Far Cry Instincts Recomp (MIT, same author: speech_volume over the language banks, GitHub #25); there the
// voices of DirectSound HLE, here the XMA decoder.
// Switches: FCP_SPEECH_VOLUME=<0..100> (percent, 100 = as the console), FCP_SPEECH_TRACK=0 (no tracking at all,
// as before), FCP_TRACE_SPEECH=1 (one log line per tracked read).
#pragma once

#include <cstdint>
#include <string>

namespace fcp::speech {

void Configure();
bool Enabled();

/** Speech volume in percent (0..100), from now on; 100 = the console's mix. */
void SetVolume(int percent);

/** After NtCreateFile/NtOpenFile succeeded: name and the handle it got. */
void OnOpen(const std::string& name, uint32_t handle);

/** Before NtReadFile(handle r3, ..., buffer r8, length r9, byte_offset* r10). */
void BeforeRead(uint8_t* base, uint32_t handle, uint32_t buffer, uint32_t length, uint32_t offset_address);

/** After NtReadFile returned status (data of BeforeRead of the same thread). */
void AfterRead(uint8_t* base, uint32_t status);

/** NtSetInformationFile(handle, iosb, info*, length, class): FilePositionInformation moves the file pointer. */
void OnSetInformation(uint8_t* base, uint32_t handle, uint32_t info_class, uint32_t info);

void OnClose(uint32_t handle);

void WriteSummary();

}  // namespace fcp::speech
