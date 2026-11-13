#!/usr/bin/env bash
# Building fcshaders: tools/get_renderer_deps.ps1 (fmt, xxHash, DXC pinned into third_party/renderer-deps).
# This script only fetches the reference sources used for the shader investigation.
# Fetches the third-party sources for tools/shaders to third_party/ (gitignored). Run from the repo root:
#   bash tools/shaders/get_third_party.sh
# Pinned (as of 2026-11-12):
#   XenosRecomp        hedge-dev/XenosRecomp 990d03b28a27b50277ee5d8d942e1c5f873869d1 (MIT); only the submodules
#                      fmt and xxHash are needed (header-only).
#   King Kong Recomp.  TekRantGaming/king-kong-recompiled e3e5d3f488cbdcd35afd0a1f08b958b0b50f03c3 (MIT); reference
#                      only, the part in use is copied into tools/shaders.
#   Xenia Canary       xenia-canary/xenia-canary 11bea29fd0a1894c4a806e2ce245780e20099005 (BSD-3), src/xenia/gpu only,
#                      reference for ucode.h.
#   DXC                microsoft/DirectXShaderCompiler v1.9.2609, dxc_2026_09_29.zip, SHA-256 below.
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p third_party
T="$PWD/third_party"

if [ ! -d "$T/XenosRecomp" ]; then
  git clone https://github.com/hedge-dev/XenosRecomp.git "$T/XenosRecomp"
fi
git -C "$T/XenosRecomp" checkout -q 990d03b28a27b50277ee5d8d942e1c5f873869d1
git -C "$T/XenosRecomp" submodule update --init thirdparty/fmt thirdparty/xxHash

if [ ! -d "$T/king-kong-recompiled" ]; then
  git clone https://github.com/TekRantGaming/king-kong-recompiled.git "$T/king-kong-recompiled"
fi
git -C "$T/king-kong-recompiled" checkout -q e3e5d3f488cbdcd35afd0a1f08b958b0b50f03c3

if [ ! -d "$T/xenia-canary" ]; then
  git clone --filter=blob:none --sparse https://github.com/xenia-canary/xenia-canary.git "$T/xenia-canary"
  git -C "$T/xenia-canary" sparse-checkout set src/xenia/gpu
fi
git -C "$T/xenia-canary" checkout -q 11bea29fd0a1894c4a806e2ce245780e20099005

D="$T/dxc-v1.9.2609"
if [ ! -f "$D/dxc/inc/dxcapi.h" ]; then
  mkdir -p "$D/dxc"
  curl -sL -o "$D/dxc_2026_09_29.zip" \
    https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip
  echo "ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1  $D/dxc_2026_09_29.zip" | sha256sum -c -
  (cd "$D/dxc" && unzip -q ../dxc_2026_09_29.zip || true)  # the warning about backslashes in the archive is harmless
  test -f "$D/dxc/bin/x64/dxcompiler.dll"
fi
echo "third_party ready"
