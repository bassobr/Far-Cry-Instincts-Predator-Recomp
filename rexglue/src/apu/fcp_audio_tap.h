/**
 * @file        apu/fcp_audio_tap.h
 *
 * FCP (local change 30, see UPSTREAM.md): measurement tap on the finished mix.
 *
 *  - FCP_RECORD_AUDIO=<file.wav>: writes the summed 6 channel mix of every period (after the title,
 *    before master gain, mute and the device down/up mix) as 48 kHz float WAV, also when apu.mute is
 *    set. A side file <file.wav>.gaps.tsv lists every period in which an open stream had no frame
 *    (the mix inserted silence for it). In a relaunched process (FCP_RELAUNCH_GEN=n) the name gets
 *    ".gen<n>" before the extension.
 *  - FCP_AUDIO_STATS=0 turns off the 10 s statistics line in the log (default on).
 *
 * Nothing here changes what is played.
 */

#pragma once

#include <rex/apu/audio_driver.h>
#include <rex/types.h>

namespace rex::apu::fcp {

/** What the mix saw in one period. */
struct PeriodInfo {
  u32 open_streams = 0;
  u32 streams_with_frame = 0;
  u32 underrun_mask = 0;  /**< bit i: slot i was open and empty */
  u32 max_queue = 0;      /**< largest queue depth before the frame was taken */
  bool device_pull = false;
};

/** Called by the mix for every period. planar: 6 channels of kAudioChannelSamples, one after another. */
void OnPeriod(const f32* planar, const PeriodInfo& info);

/** Flushes and closes the recording (mix stopped). Safe to call more than once. */
void Shutdown();

}  // namespace rex::apu::fcp
