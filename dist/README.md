# Far Cry Instincts Predator for Windows (static recompilation)

A native Windows port of the Xbox 360 game *Far Cry Instincts Predator* (both campaigns, *Instincts* and
*Evolution*), made by statically recompiling the game's PowerPC code to x86-64 on the ReXGlue SDK, with a native
Vulkan/Direct3D 12 renderer.

**This download contains no part of the game.** It has no game code, no game data and no shaders of the game. You need
your own copy of the game: a disc image (`.iso`) of the retail disc *Far Cry Instincts – Predator* (Xbox 360,
USA/Europe, without a title update). The game code is translated from your disc image on your PC the first time you
start the game.

## Requirements

- Windows 10 or 11, 64-bit; a CPU with AVX2 (x86-64-v3: Intel Haswell / AMD Zen or newer).
- A GPU with Vulkan 1.3 or Direct3D 12.
- About 3 GB of free disk space for the first start (the prepared game code takes about 60 MB afterwards).
- Your disc image of the game (see above). The file `default.xex` in it must have the SHA-256
  `0ABD35C4E4C4F39DD330CEBFE051D28A313EF2BDCD80921CACA8F1F2956B0B56`.

## First start

1. Extract the whole zip into a folder you can write to (for example `Documents\Games`).
2. Start `predator.exe` once. It creates the user folder `%LOCALAPPDATA%\Far-Cry-Instincts-Predator-Recomp` with the
   settings file `predator.toml` and tells you to set your disc image.
3. Open `predator.toml` and set the path of your disc image:

   ```toml
   [game]
   image = "D:/Games/Far Cry Instincts Predator.iso"
   ```

4. Start `predator.exe` again. A window shows how the game is prepared for your PC, once:
   - reading `default.xex` from the disc image and checking it,
   - translating it to C++ (`compiler\bin\rexglue.exe`),
   - compiling and linking it (`compiler\bin\clang.exe`, `compiler\bin\lld-link.exe`) into
     `%LOCALAPPDATA%\Far-Cry-Instincts-Predator-Recomp\game_code\predator_code.dll`,
   - translating the game's shaders into a shader pack (`fcshaders.exe`).

   This took about 1 minute on a 16-core PC and about 3 minutes limited to 4 cores / 8 threads; slower CPUs take longer. The game starts by itself
   afterwards. Later starts take a few seconds. If you close the window, what is done so far is kept and the next
   start carries on.

A new release that changes the recompiler or the hooks prepares the game code again on its first start; nothing else
has to be done.

## Files

| Where | What |
|---|---|
| `predator.exe` | the game's runtime: kernel and XAM emulation, renderer, audio, input, the first-start build |
| `rex*.dll`, `SDL3.dll` | runtime libraries (ReXGlue SDK, SDL 3) |
| `rexffmpeg.dll` | FFmpeg decoders for XMA audio (LGPL 2.1, see `THIRD_PARTY_NOTICES.md`) |
| `dxcompiler.dll`, `fcshaders.exe`, `renderer.toml` | shader translation (DirectX Shader Compiler) and renderer layout |
| `vcruntime140*.dll`, `msvcp140*.dll` | Microsoft Visual C++ runtime |
| `compiler\` | clang, lld-link and rexglue for the first start, with the headers and libraries they need |
| `compiler\pgo\predator.profdata` | profile for the optimisation of the game code (PGO): function names of the form `sub_<address>` and execution counts from test runs, no code or data of the game |
| user folder | `%LOCALAPPDATA%\Far-Cry-Instincts-Predator-Recomp`: settings, saves, logs, caches, the game code |

To keep everything next to `predator.exe` instead (portable), create an empty file `portable.txt` beside it.

## If something goes wrong

- The first-start window shows the end of `game_code\build.log` when a step fails; the full log is in the user folder.
- `predator.log` in the user folder records every start.
- The game must be started as `predator.exe` (the game code links against that name).

## License

This port's own code is released under the MIT License (`LICENSE`). Third-party components and their licenses are listed in
`THIRD_PARTY_NOTICES.md`. *Far Cry* is a trademark of Ubisoft Entertainment; this project is not affiliated with or
endorsed by Ubisoft or Microsoft.
