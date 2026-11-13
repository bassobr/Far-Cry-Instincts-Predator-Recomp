#!/usr/bin/env bash
# Fetches the third-party sources for tools/renderer-probe to third_party/ (gitignored). Run from the repo root:
#   bash tools/renderer-probe/get_third_party.sh
# Pinned (as of 2026-11-12):
#   Vulkan-Headers  KhronosGroup/Vulkan-Headers  tag v1.3.296 (Apache-2.0 OR MIT)
#   volk            zeux/volk                    tag 1.3.295 (MIT): loads vulkan-1.dll without the Vulkan SDK
#   King Kong Rec.  TekRantGaming/king-kong-recompiled e3e5d3f (MIT): reference for the analysis only
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p third_party
T="$PWD/third_party"

if [ ! -d "$T/Vulkan-Headers" ]; then
  git clone --depth 1 --branch v1.3.296 https://github.com/KhronosGroup/Vulkan-Headers.git "$T/Vulkan-Headers"
fi
if [ ! -d "$T/volk" ]; then
  git clone --depth 1 --branch 1.3.295 https://github.com/zeux/volk.git "$T/volk"
fi
if [ ! -d "$T/king-kong-recompiled" ]; then
  git clone https://github.com/TekRantGaming/king-kong-recompiled.git "$T/king-kong-recompiled"
fi
echo done
