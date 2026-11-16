# Texture packs

The game's textures can be written out as image files and replaced with your own images, for example upscaled or
retouched ones. Both are off by default; with both off nothing of this runs.

```toml
# predator.toml (user folder)
[graphics]
texture_dump = false     # true: write every texture the game uses into textures\dump\
texture_replace = true   # true: draw images from textures\replace\ instead of the game's textures
```

Both keys need a restart. The same switches as environment variables: `FCP_TEXTURE_DUMP=1`, `FCP_TEXTURE_REPLACE=1`.
The folders are below the user folder (the folder with `predator.toml` and `predator.log`):
`textures\dump\` and `textures\replace\`.

Dumped textures are data of the game. Keep them for yourself; share only images you made.

## Workflow

1. Set `texture_dump = true`, start the game and visit the places whose textures you want to change.
2. Copy the files you want from `textures\dump\` and edit or upscale them. Keep the 16-digit hash at the start of
   the name.
3. Put the results into `textures\replace\` (subfolders are fine), set `texture_replace = true` (and
   `texture_dump = false`) and start the game. The log shows each replacement it loads
   (`texture pack: <hash> replaced by <file> ...`).

While the game runs, new, changed and removed files in `textures\replace\` are picked up about half a second after
the last change; the texture changes on screen a few frames later. Until a replacement is loaded and uploaded, the
game's own texture is drawn; loading happens on a background thread.

## File names

A dumped file is called `<hash>_<width>x<height>_<format>.<png|dds>`, for example
`3f0c2a9d81e4b756_256x256_dxt1.dds` or `9a41d0e2c3b87f15_128x64_8888.png`; cube maps end in `_cube.dds`.

- `<hash>` is a 64-bit hash (16 hex digits) of the texture's top level as the game stores it, together with its
  format and size. It does not depend on where the texture lies in memory, so the same texture has the same name in
  every run, every level and on every PC.
- A replacement is found by the first part of its name that is 16 hex digits: `<hash>.png`, `<hash>.dds`,
  `<hash>_crate.png` and the full dump name all work. With two files for the same hash, the first in path order wins.
- `textures\dump\index.txt` lists every dumped file with size, the console's format, the levels and the swizzle.
  Files already in the dump folder (or with a replacement) are not written again, so several runs add to it.

## Formats

| The game's texture | Dumped as | Replacement |
|---|---|---|
| block compressed (DXT1, DXT3, DXT5, DXN, DXT5A) | `.dds` with the game's blocks unchanged and all of its mip levels; channels in the console's storage order | DDS (BC1-BC5, BC7, RGBA8/BGRA8, with or without mips) or PNG, in the same channel order as the dump |
| uncompressed 8-bit colour (8888, 565, 1555, 4444, 8, 88) and small DXT textures whose size is not a multiple of 4 | `.png`, top level, as the shader sees it (channels already in the game's order) | PNG or DDS as above, in the order of the PNG |
| cube maps | `.dds` with 6 faces (blocks unchanged, or RGBA8 as seen) | a cube DDS only; a 2D image for a cube map (or the other way round) is refused with a log line |

- **Any resolution.** A replacement may be larger (or smaller) than the original; it covers the same area. The aspect
  ratio should stay the same. The game's filtering and addressing stay as they are.
- **Mipmaps.** A PNG (or a DDS with RGBA8 and one level) gets a full mip chain made with a 2x2 box filter. A DDS
  with its own mips keeps them; a block compressed DDS with one level stays at one level (it may shimmer at a
  distance; save it with mips).
- **Block compression.** BC images need a top level whose width and height are multiples of 4. BC7 works for every
  texture; for normal maps and alpha keep the channel layout of the dump. sRGB DDS formats are read as their UNORM
  counterpart (the game does its own gamma).
- **Alpha.** The alpha channel is used as it is. Where the game ignores alpha (opaque formats), it keeps ignoring it.

## What is not dumped or replaced

- Render targets and resolve targets (the picture itself, shadow maps, reflections, post-processing), and textures
  the game keeps rewriting (videos): they never become files.
- Volume (3D) textures, texture arrays, depth textures, video (Y'CbCr) formats, signed, float, integer and
  16-bit-per-channel formats, and views of a texture that do not start at its top level. The log names each reason
  once (`texture pack: textures not dumped or replaced: ...`).
- The button prompt pages of the menu font (keyboard and controller symbols, `[input] button_prompts`) are drawn by
  the prompt module; a replacement for those pages is used only with `button_prompts = "xbox"`.

## Cost

With both keys off nothing of this runs. Dumping converts each texture a second time (on the game's render thread,
at most 32 MB per frame) and writes the files on a background thread; expect slower loading while dumping. With
replacements, each texture the game loads is hashed once, and replacement images are uploaded with at most 16 MB per
frame (`FCP_TEXTURE_UPLOAD_MB`), so a large pack spreads its uploads over a few frames instead of stalling one.

Measured on a desktop PC (Ryzen 9 7950X3D, RTX 4070 Ti SUPER) in `21_dunea` at 120 frames/s, one run each: with
every one of the level's ~790 textures replaced (285 MB uploaded) and with dumping on, the level loaded 0.1 s
later (3.1 s instead of 3.0 s) and kept 120 frames/s with the same frame times as without (p99 about 9 ms). Naming
all textures of the level took about 1 ms in total. PNG replacements are stored uncompressed on the GPU (4 bytes per
texel plus mips); for large packs prefer BC7 or BC1/BC3 DDS files.
