# Releasing

A checklist for a release. **A release never contains game code or game data**: no `default.xex`, no
`predator_code.dll`, no generated code, no shader packs or caches, no saves, logs or settings files, and no PGO
profile in the public build. `tools\dist.ps1` refuses all of these, and the release workflow checks the zip again.

## 1. Version

- Set the new version in `VERSION` (for example `0.1.1`); the tag will be `v<VERSION>`.
- Add a section for it at the top of `CHANGELOG.md`: features and known limitations, measured statements only.
- Update the version line in `README.md` if it names one.

## 2. Build the release folder

From a clean checkout, with the one-time setup of [BUILDING.md](BUILDING.md) done and **without** a disc image
(as the CI does):

```bat
cmd /c tools\dist.cmd release
```

Result in `out\dist\`: `Far-Cry-Instincts-Predator-Recomp-v<version>-win64\` and its zip,
`Far-Cry-Instincts-Predator-Recomp-v<version>-sources.zip` and `SHA256SUMS`.

## 3. Checks

- **No game content.** `dist.ps1` ended without "refusing to package". The zip has no `predator_code*`, no
  `game_code\`, `generated\` or `shaders\` folder, no `.xex`, `.iso`, `.fcpack`, `.pdb`, `.obj`, `.profdata`, `.log`,
  no `predator.toml` or `portable.txt`, and no EXE/DLL with a PE section `rexguest`.
- **SHA256SUMS** lists exactly the release zip and the source archive, and the hashes match:
  `certutil -hashfile <file> SHA256` (or `sha256sum -c SHA256SUMS`).
- **Source archive** holds `BUILDING.txt`, `ffmpeg/source/`, `libmspack/source/`, `binutils-disasm/source/`,
  `build-recipe/` and `licenses/` (GPL-2.0, LGPL-2.1), with the commits that `tools\get_deps.ps1` pins.
- **First start** of the extracted release folder with a disc image and an empty user folder (`FCP_USER_ROOT`):
  the game code and the shader pack are built, the game reaches a level. `tools\disttest.ps1` automates this.
- **Licences:** every text named in `dist\THIRD_PARTY_NOTICES.md` is in `licenses\` of the release folder.

## 4. Tag and CI

- Commit, then create the tag `v<VERSION>` on that commit and push the commit and the tag.
- The workflow `.github/workflows/release.yml` runs on the tag: it checks that the tag matches `VERSION`, builds the
  `dist` target without a disc image, checks the package and the source archive, attests the zips and creates a
  **draft** release.

## 5. Release assets

- The draft holds `Far-Cry-Instincts-Predator-Recomp-v<version>-win64.zip`,
  `Far-Cry-Instincts-Predator-Recomp-v<version>-sources.zip` and `SHA256SUMS`. Nothing else is uploaded.
- Edit the generated notes (take the `CHANGELOG.md` section; mention that the player needs their own disc image),
  then publish the draft by hand.
