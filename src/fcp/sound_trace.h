// FCP_TRACE_SOUND_READS=1: logs every NtReadFile on a handle of dvdsound.dat ("sound read t=… off=…
// len=…"), so tools/audio/sound_reads.py can map the reads to the archive entries (XMA banks or the
// IMA-ADPCM streams). Measurement only.
#pragma once

#include <cstdint>
#include <string>

namespace fcp::sound_trace {

void Configure();
bool Enabled();

/** After NtCreateFile/NtOpenFile: name of the opened file and the handle it got (result 0). */
void OnOpen(const std::string& name, uint32_t handle);

/** Before NtReadFile(handle, …, length r9, byte_offset* r10). */
void OnRead(uint32_t handle, uint32_t length, uint8_t* base, uint32_t offset_address, uint32_t lr);

/** Before NtSetInformationFile(handle, iosb, info*, length, class): FilePositionInformation moves the
 *  file pointer that a read without byte offset uses. */
void OnSetInformation(uint32_t handle, uint32_t info_class, uint8_t* base, uint32_t info);

/** After NtClose(handle). */
void OnClose(uint32_t handle);

void WriteSummary();

}  // namespace fcp::sound_trace
