# fcshaders: origin and changes

License: MIT. The texts are included: `LICENSE-XenosRecomp.md` (hedge-dev and contributors) in this folder,
`../LICENSE-KingKongRecompiled.txt` (TekRantGaming and contributors).

## Origin

| File under `tools/shaders/` | Source | Version |
|---|---|---|
| `xenosrecomp/shader_recompiler.cpp`, `.h`, `shader.h`, `shader_code.h`, `constant_table.h`, `pch.h` | XenosRecomp (github.com/hedge-dev/XenosRecomp, commit `990d03b`), in the revision by King Kong Recompiled (`native-renderer/thirdparty/XenosRecomp/XenosRecomp/`) | KK commit `e3e5d3f` (2026-11-09) |
| `include/fcshaders/container.h`, `src/container.cpp` | KK `native-renderer/shaders/include/kkshaders/container.h`, `src/container.cpp` | `e3e5d3f` |
| `include/fcshaders/translator.h`, `src/translator.cpp` | KK `.../kkshaders/translator.h`, `src/translator.cpp` | `e3e5d3f` |
| `include/fcshaders/compiler.h`, `src/compiler.cpp`, `include/fcshaders/abi.h` | KK `.../kkshaders/compiler.h`, `src/compiler.cpp`, `abi.h` | `e3e5d3f` |
| `hlsl/fc_common.hlsli` | KK `native-renderer/shaders/hlsl/kk_common.hlsli`, unchanged (identifiers `kk_*` kept) | `e3e5d3f` |
| `tool/main.cpp`, `CMakeLists.txt`, `build.cmd`, `*.py`, `get_third_party.sh` | original code of this project | |

## What King Kong changed relative to XenosRecomp (per KK `thirdparty/NOTES.md` and `docs/shaders.md`)

- Container reading moved out of the core (`RecompilerInput`), container adapter for the early container
  `0x102A0E00/01` instead of `0x102A1100/01`.
- Bindless resources, one layout for all shaders (`abi.h`), vertex pulling from guest memory (big-endian) with
  format/offset/stride from draw constants instead of a host input layout and specialisation constants.
- Full constant and control-flow model (bool and loop constants, predicates, jumps, calls,
  aL/a0-relative addressing), literal constants answered in the shader (`kkConstRel`), bool literals inlined.
- One `if` block per run of identically predicated instructions, `select` instead of `?:`/`&&`/`||`.
- Eleven fixes from differential tests against a reference interpreter (cube offsets, cube face, vector/scalar
  order for a0/p0, aL clamping, loop repeat, unordered `!=`, `vfetch_mini`, multiplication rule,
  DXC `-Gis`, `log/sqrt/rsq` of negative inputs, bool literals).
- King Kong's own switches: render scaling, ambient-only AO, water reflection, parallax, motion variant.
  They remain in the core (off = byte-identical output according to KK); fcshaders never sets them.

## Changes in fcshaders (Predator)

1. `shader_recompiler.cpp`: interpolator signature extended from 18 to 20 slots; slots 18/19 carry
   `POSITION1`/`POSITION2` (HLSL semantics `FCPOS1`/`FCPOS2`). Reason: `lake` and `ocean` pass these
   semantics from the VS to the PS; before, 6 shaders failed with "unsupported interpolator semantic". Affects all
   shaders (the signature is the same for all), but changes no previously translatable computation.
2. `container.h/.cpp`: namespace `fcshaders`; KK's database `xeshaders.bin` (`Database`) and the
   KK switch fields in `ShaderInfo` removed. Parsing a container is unchanged.
3. `translator.h/.cpp`: KK switches (render scaling, AO, water reflection, parallax) and
   `makeMotionVertexShader` removed; HLSL header line changed. Input hash and bindings unchanged.
4. `compiler.h/.cpp`, `abi.h`: namespace and header comment only.
5. `translator.h`: comment on `ShaderBindings::interpolators` extended by bits 18-19.
6. `shader_recompiler.cpp`: `FOG0` (oFog of the vs.1.1 shaders assembled at runtime, e.g.
   `primtemplates\basic.vsh`) shares slot 15 with `TEXCOORD15`. No Predator shader uses `TEXCOORD15` (disc:
   TEXCOORD0-14, vs.1.1 only knows oT0-oT7); the signature of all other shaders stays the same. If a VS names both,
   it fails with "interpolator slot collision". Before, 3 runtime VS failed with "unsupported interpolator
   semantic FOG0". `kTranslatorVersion` 6.
7. `translator.cpp`: `renderScaleAware` always on (KK switch reinstated, without an option): the pixel position and
   the sizes read with `GetDimensions` (unnormalised coordinates, fetch offsets, `getWeights`) go through
   `KKScaleConstants` (b3) into guest units. At factor 1 the result is the same. `kTranslatorVersion` 7
   (pack and disk cache are rebuilt). Purpose: rendering at an internal resolution above the guest's.
