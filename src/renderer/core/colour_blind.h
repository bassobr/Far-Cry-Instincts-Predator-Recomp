// [graphics] colour_blind: the colour matrix of the daltonization in the final blit (kBlitColourPS).
// Taken from Far Cry Instincts Recomp (MIT, same author; src/gpu/present.cpp, which names SCDA-XBERecomp as its
// source): Machado, Oliveira and Fernandes 2009 simulation at full severity, then Fidaner's daltonize, which moves the
// difference the eye loses into channels it still sees: M = I + shift * (I - simulate). Applied to linear colour
// (gamma 2.2), clamped to 0..1 before and after. White and greys stay (every row of M sums to 1).
#pragma once

namespace fcp::nr {

// 0 off, 1 protanopia, 2 deuteranopia, 3 tritanopia
inline void ColourBlindMatrix(int mode, float m[3][4]) {
  static constexpr float kSimulate[3][9] = {
      {0.152286f, 1.052583f, -0.204868f, 0.114503f, 0.786281f, 0.099216f, -0.003882f, -0.048116f, 1.051998f},
      {0.367322f, 0.860646f, -0.227968f, 0.280085f, 0.672501f, 0.047413f, -0.011820f, 0.042940f, 0.968881f},
      {1.255528f, -0.076749f, -0.178779f, -0.078411f, 0.930809f, 0.147602f, 0.004733f, 0.691367f, 0.303900f},
  };
  static constexpr float kShift[3][9] = {
      {0, 0, 0, 0.7f, 1, 0, 0.7f, 0, 1},
      {0, 0, 0, 0.7f, 1, 0, 0.7f, 0, 1},
      {1, 0, 0.7f, 0, 1, 0.7f, 0, 0, 0},
  };
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c) m[r][c] = c == r ? 1.0f : 0.0f;
  if (mode < 1 || mode > 3) return;
  const float* sim = kSimulate[mode - 1];
  const float* shift = kShift[mode - 1];
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) {
      float lost = 0;
      for (int k = 0; k < 3; ++k) lost += shift[r * 3 + k] * (float(k == c) - sim[k * 3 + c]);
      m[r][c] = float(r == c) + lost;
    }
}

inline const char* ColourBlindName(int mode) {
  static const char* const kNames[] = {"off", "protanopia", "deuteranopia", "tritanopia"};
  return mode >= 0 && mode <= 3 ? kNames[mode] : "off";
}

}  // namespace fcp::nr
