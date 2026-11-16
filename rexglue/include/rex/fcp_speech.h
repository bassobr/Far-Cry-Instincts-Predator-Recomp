/**
 * @file        fcp_speech.h
 *
 * @license     BSD 3-Clause License (FCP local change 370, see UPSTREAM.md)
 *
 * @remarks     FCP: speech volume. The title layer registers the sound data it sees the game read (speech or
 *              other sound) by content: a hash of kWindow bytes where an XMA sound starts. The
 *              game copies its reads out of a staging buffer into the place the XMA decoder reads from, so
 *              addresses do not identify the data; the bytes do. For every decoded frame the XMA decoder looks
 *              up the hash at the start of its input buffer and scales the frame by the gain when it is speech.
 *              With the gain at 1 (the default) nothing changes.
 */

#pragma once

#include <rex/platform.h>
#include <rex/types.h>

namespace rex::fcp_speech {

/** What a piece of data is, as the title layer registered it. */
enum class DataKind : u8 {
  kUnknown = 0,  /**< not registered */
  kSound = 1,    /**< sound data that is not speech (or the same bytes in both kinds) */
  kSpeech = 2,   /**< speech */
};

/**
 * Bytes hashed per position, and the spacing of the positions (from the start of a sound entry). Only positions that
 * look like the first XMA packet of a sound are registered (the first frame starts right after the packet header):
 * where the XMA data of a bank begins differs from bank to bank (measured: 344 and 1148 modulo 2048, 4 byte aligned).
 */
inline constexpr u32 kWindow = 64;
inline constexpr u32 kGrain = 4;
/** Diagnostics: the entry offset of a match modulo 2048 is counted in steps of this many bytes. */
inline constexpr u32 kPhaseStep = 128;

/** Gain on decoded speech, 0 to 1; 1 = as the console (the decoder's path is then unchanged). */
REX_API void SetGain(f32 gain);
REX_API f32 Gain();

/**
 * Registers data of one sound entry (host pointer to a copy the caller holds during the call).
 *
 * @param entry_offset Offset of data[0] from the start of its entry; positions are where the offset
 *        from the entry start is a multiple of kGrain.
 */
REX_API void Register(const u8* data, u32 size, u32 entry_offset, DataKind kind);

/**
 * The kind of the data at a host pointer (kWindow readable bytes), kUnknown when not registered.
 *
 * @param phase Receives the entry offset of the registered position modulo 2048 (diagnostics).
 */
REX_API DataKind KindOf(const u8* data, u32* phase = nullptr);

/** Registered positions of each kind (diagnostics). */
REX_API void Registered(u64* sound, u64* speech);

}  // namespace rex::fcp_speech
