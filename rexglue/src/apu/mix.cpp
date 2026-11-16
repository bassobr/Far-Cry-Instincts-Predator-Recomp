#include "apu/mix.h"

#include <algorithm>
#include <cmath>

namespace rex::apu {

void MixSurround(f32* output, const f32* planar, size_t count, const SurroundWeights& weights,
                 f32 gain, bool clamp) {
  const f32 level[6] = {gain,
                        gain,
                        weights.center * gain,
                        weights.lfe * gain,
                        weights.surround * gain,
                        weights.surround * gain};
  for (size_t channel = 0; channel < 6; ++channel) {
    const f32* in = planar + channel * count;
    f32* out = output + channel;
    const f32 weight = level[channel];
    if (!clamp) {  // FCP: change 371
      for (size_t n = 0; n < count; ++n) {
        out[n * 6] = in[n] * weight;
      }
      continue;
    }
    for (size_t n = 0; n < count; ++n) {
      out[n * 6] = std::clamp(in[n] * weight, -1.0f, 1.0f);
    }
  }
}

void MixStereo(f32* output, const f32* planar, size_t count, const StereoWeights& weights,
               f32 gain, bool clamp) {
  const f32* fl = planar;
  const f32* fr = planar + count;
  const f32* fc = planar + 2 * count;
  const f32* lf = planar + 3 * count;
  const f32* bl = planar + 4 * count;
  const f32* br = planar + 5 * count;
  const f32 scale = weights.scale * gain;
  for (size_t n = 0; n < count; ++n) {
    const f32 mid = fc[n] * weights.center + lf[n] * weights.lfe;
    const f32 left = (fl[n] + mid + bl[n] * weights.surround) * scale;
    const f32 right = (fr[n] + mid + br[n] * weights.surround) * scale;
    output[n * 2] = clamp ? std::clamp(left, -1.0f, 1.0f) : left;  // FCP: change 371
    output[n * 2 + 1] = clamp ? std::clamp(right, -1.0f, 1.0f) : right;
  }
}

// FCP: change 371, apu.limiter (mix.h).
void PeakLimiter::Reset(u32 channels) {
  channels_ = std::clamp<u32>(channels, 1, kAudioFrameChannels);
  pos_ = 0;
  release_ = 1.0f;
  std::fill(std::begin(delay_), std::end(delay_), 0.0f);
  std::fill(std::begin(need_), std::end(need_), 1.0f);
  std::fill(std::begin(held_), std::end(held_), 1.0f);
}

void PeakLimiter::Process(f32* samples, size_t frames, LimiterStats& stats) {
  const u32 ch = channels_;
  // one step per frame towards 1 with the release time constant
  // (in double: near 1 the steps are below the float resolution, the gain would stay just under 1)
  const f64 recover = 1.0 - std::exp(-1.0 / (f64{kReleaseSeconds} * kAudioSampleRate));
  for (size_t n = 0; n < frames; ++n) {
    f32* x = samples + n * ch;
    f32 peak = 0.0f;
    for (u32 c = 0; c < ch; ++c) {
      const f32 a = std::fabs(x[c]);
      peak = std::max(peak, a);
      stats.over_in += a > 1.0f ? 1 : 0;
    }
    stats.peak_in = std::max(stats.peak_in, peak);
    const u32 now = pos_;
    const u32 oldest = (pos_ + 1) % kLookahead;
    need_[now] = peak > kCeiling ? kCeiling / peak : 1.0f;
    f32 held = 1.0f;
    for (f32 v : need_) {
      held = std::min(held, v);
    }
    held_[now] = held;
    f32 sum = 0.0f;
    for (f32 v : held_) {
      sum += v;
    }
    // exactly 1 when every value is 1 (a sum of kLookahead ones is exact)
    const f32 smooth = sum / static_cast<f32>(kLookahead);
    f64 released = std::min<f64>(smooth, release_ + (1.0 - release_) * recover);
    if (released > 1.0 - 1e-6 && smooth == 1.0f) {
      released = 1.0;
    }
    release_ = released;
    const f32 gain = static_cast<f32>(released);
    // out: the frame kLookahead - 1 frames back; in: this frame
    f32* delayed = delay_ + oldest * kAudioFrameChannels;
    f32* slot = delay_ + now * kAudioFrameChannels;
    for (u32 c = 0; c < ch; ++c) {
      slot[c] = x[c];
    }
    if (gain < 1.0f) {
      ++stats.limited_frames;
      stats.min_gain = std::min(stats.min_gain, gain);
    }
    for (u32 c = 0; c < ch; ++c) {
      const f32 out = gain == 1.0f ? delayed[c] : delayed[c] * gain;
      x[c] = out;
      const f32 a = std::fabs(out);
      stats.peak_out = std::max(stats.peak_out, a);
      stats.over_out += a > 1.0f ? 1 : 0;
    }
    pos_ = oldest;
  }
}

void RampGain(f32* planar, size_t count, f32 from, f32 to) {
  const f32 step = (to - from) / static_cast<f32>(count);
  for (size_t channel = 0; channel < 6; ++channel) {
    f32* samples = planar + channel * count;
    f32 level = from;
    for (size_t n = 0; n < count; ++n) {
      level += step;
      samples[n] *= level;
    }
  }
}

void StreamGuard::Apply(std::span<f32, rex::apu::kAudioFrameSamples> frame, f64 peak_limit) {
  f32 peak = 0.0f;
  bool finite = true;
  for (f32 sample : frame) {
    if (!std::isfinite(sample)) {
      finite = false;
      break;
    }
    peak = std::max(peak, std::fabs(sample));
  }
  if (!finite) {
    std::ranges::fill(frame, 0.0f);
    over_periods_ = kPeakPeriods;
    silenced_ = true;
    applied_ = 0.0f;
    return;
  }
  if (peak_limit > 0.0 && peak > static_cast<f32>(peak_limit)) {
    if (++over_periods_ >= kPeakPeriods) {
      silenced_ = true;
    }
  } else {
    over_periods_ = 0;
    silenced_ = false;
  }
  const f32 target = silenced_ ? 0.0f : 1.0f;
  if (applied_ != 1.0f || target != 1.0f) {
    const f32 step = (target - applied_) / static_cast<f32>(rex::apu::kAudioChannelSamples);
    for (size_t channel = 0; channel < rex::apu::kAudioFrameChannels; ++channel) {
      f32 level = applied_;
      for (size_t n = 0; n < rex::apu::kAudioChannelSamples; ++n) {
        frame[channel * rex::apu::kAudioChannelSamples + n] *= level;
        level += step;
      }
    }
  }
  applied_ = target;
}

}  // namespace rex::apu
