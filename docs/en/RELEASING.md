# Releasing

A checklist for a release. **A release never contains game code or game data**: no `default.xex`, no
`predator_code.dll`, no generated code, no shader packs or caches, no saves, logs or settings files. The one file derived from the game is the PGO
profile `config/predator/game_code.profdata` (function names, structure hashes and execution counts, no code), shipped
as `compiler\pgo\predator.profdata`. `tools\dist.ps1` refuses everything else, and the release workflow checks the zip
again.

## 1. Version

- Set the new version in `VERSION` (for example `0.1.1`); the tag will be `v<VERSION>`.
- Add a section for it at the top of `CHANGELOG.md`: features and known limitations, measured statements only.
- Update the version line in `README.md` if it names one.

## 2. PGO profile

- Retrain the profile when the build's PGO check warned (`pgo_check: WARNING` in the build log) or after a change
  to the code generator, the title configuration (`config/predator/*.toml`), the hooks or the LLVM version:
  `powershell -File tools\pgo.ps1` (needs the disc image; [BUILDING.md](BUILDING.md#pgo-profile)). Commit the new
  `config/predator/game_code.profdata`.
- A build with the current profile prints `pgo_check: ... stale share` below the threshold.

## 3. Build the release folder

From a clean checkout, with the one-time setup of [BUILDING.md](BUILDING.md) done and **without** a disc image
(as the CI does):

```bat
cmd /c tools\dist.cmd release
```

Result in `out\dist\`: `Far-Cry-Instincts-Predator-Recomp-v<version>-win64\` and its zip,
`Far-Cry-Instincts-Predator-Recomp-v<version>-sources.zip` and `SHA256SUMS`.

## 4. Checks

- **No game content.** `dist.ps1` ended without "refusing to package". The zip has no `predator_code*`, no
  `game_code\`, `generated\` or `shaders\` folder, no `.xex`, `.iso`, `.fcpack`, `.pdb`, `.obj`, `.log`, no
  `predator.toml` or `portable.txt`, and no EXE/DLL with a PE section `rexguest`.
- **PGO profile:** exactly one `.profdata`, `compiler\pgo\predator.profdata`, with the same SHA-256 as
  `config/predator/game_code.profdata` (`dist.ps1` and the release workflow check it). `predator.exe` only uses
  a profile with the SHA-256 it was built with; the first-start log shows `game code: PGO profile ...`.
- **SHA256SUMS** lists exactly the release zip and the source archive, and the hashes match:
  `certutil -hashfile <file> SHA256` (or `sha256sum -c SHA256SUMS`).
- **Source archive** holds `BUILDING.txt`, `ffmpeg/source/`, `libmspack/source/`, `binutils-disasm/source/`,
  `build-recipe/` and `licenses/` (GPL-2.0, LGPL-2.1), with the commits that `tools\get_deps.ps1` pins.
- **First start** of the extracted release folder with a disc image and an empty user folder (`FCP_USER_ROOT`):
  the game code and the shader pack are built, the game reaches a level. `tools\disttest.ps1` automates this.
- **Licences:** every text named in `dist\THIRD_PARTY_NOTICES.md` is in `licenses\` of the release folder.

## 5. Tag and CI

- Commit, then create the tag `v<VERSION>` on that commit and push the commit and the tag.
- The workflow `.github/workflows/release.yml` runs on the tag: it checks that the tag matches `VERSION`, builds the
  `dist` target without a disc image, checks the package and the source archive, attests the zips and creates a
  **draft** release.

## 6. Release assets

- The draft holds `Far-Cry-Instincts-Predator-Recomp-v<version>-win64.zip`,
  `Far-Cry-Instincts-Predator-Recomp-v<version>-sources.zip` and `SHA256SUMS`. Nothing else is uploaded.
- Edit the generated notes (take the `CHANGELOG.md` section; mention that the player needs their own disc image),
  then publish the draft by hand.
