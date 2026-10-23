# Into the Breach: Native Save & Load

**v0.4.7 · by dherorunner · MIT**

[中文](README.md) · [![build](https://github.com/dherorunner/itb-native-save-load/actions/workflows/build.yml/badge.svg)](https://github.com/dherorunner/itb-native-save-load/actions/workflows/build.yml)

An unofficial mod that saves a battle and loads it back from inside the game: press
**Esc → Save / Load**. It calls the game's own save/load functions, so no Python, console or external
save tool is needed while playing. The game and ModLoader must be installed separately; neither is
distributed here.

## Scope

- One temporary manual slot and one pre-deployment automatic save.
- Loading works only **within the same game process and the same battle**. After a restart, an extra
  slot that is still on disk cannot be loaded by this version, and loading across battles is not
  supported.
- The game's own "Continue" and this mod's temporary slot serve different purposes.

0.4.7 passed offline checks plus validation of menu layout, representative buttons and the SL entry
toggle. The in-game evidence for the manual/auto round trip is a single case from 0.4.6 (the save/load
business source is unchanged and this package reuses the v5 candidate DLL byte-for-byte);
**0.4.7 did not re-test the full round trip in game**. Full boundaries: [VALIDATION.md](VALIDATION.md) (Chinese).

## For players: install

1. Close the game and back up your whole user data directory — see [INSTALL.zh-CN.md](INSTALL.zh-CN.md)
   (Chinese).
2. If ModLoader is not installed, install
   [ITB-ModLoader 2.9.6](https://github.com/itb-community/ITB-ModLoader/releases/tag/v2.9.6) following the
   [official instructions](https://github.com/itb-community/ITB-ModLoader/tree/v2.9.6#installation).
3. Download `ITB-Native-Save-Load-v0.4.7-windows.zip` from Releases, unzip it and copy the
   **`itb_sl` folder** into the game's `mods` directory, then enable it in ModLoader's mod management
   screen.

The result should contain `Into the Breach/mods/itb_sl/scripts/init.lua` and
`Into the Breach/mods/itb_sl/native/itb_sl_p04.dll`, with no extra nesting level.
GitHub's auto-generated "Source code" archives are not installable mods.

## For developers: build from source

Requires Windows, Python 3.12 (standard library only) and Visual Studio 2022 / Build Tools with the
MSVC x86/x64 C++ toolset (`vswhere.exe` must exist). No game files, third-party Python packages or real
saves are needed.

```powershell
python -X utf8 -B tools/build_mod.py
```

The output goes to `build/<timestamp>/package/itb_sl`, together with a source-build ZIP. Recompiling is
not guaranteed to reproduce the released DLL byte-for-byte. See [BUILDING.md](BUILDING.md) for details
(Chinese); every push to `main` is also compiled on a clean Windows runner by GitHub Actions.

## Repository layout

| Path | Contents |
|---|---|
| `src/mod/scripts/` | Lua modules: entry point, reader, save store, controller, UI, Esc menu |
| `src/native/` | C++ native bridge and file service, compiled into `itb_sl_p04.dll` |
| `tools/build_mod.py` | Standalone builder: compiles the DLL and packages the Lua modules and docs |
| `release.json`, `SOURCE_PROVENANCE.json` | Release metadata and export/source inventory |
| `*.md` | Install, limitations, build, validation and third-party notices |

## Requirements

| Item | Scope |
|---|---|
| OS / input | Windows desktop build; keyboard and mouse; tested on the Windows Steam build |
| Game | 1.2.93 in the verified environment; the EXE must also match the SHA-256 below |
| Loader | ITB-ModLoader 2.9.6 with its own UI and extensions; no other mod dependency |
| In-game UI | Chinese only; the bilingual name does not mean an English UI is provided |
| Save/load scope | Same process, same battle; one manual slot and one auto save |

Supported `Breach.exe` SHA-256:

```text
31fe352655982398fb3ee8b0bbe80efd5d65e3a9aa11e3dc39d0364354493fe9
```

The same version string does not guarantee the same EXE. The mod disables its save/load service on a
mismatched EXE; do not bypass the hash check. No extra Python, compiler or VC runtime is needed to play.

## Documentation

- [Install, upgrade, uninstall and backup](INSTALL.zh-CN.md)
- [Known limitations and troubleshooting](KNOWN_ISSUES.zh-CN.md)
- [Changelog](CHANGELOG.md) · [Release notes for this version](RELEASE_NOTES.md)
- [Validation scope](VALIDATION.md) · [Build instructions](BUILDING.md) ·
  [Third-party notices](THIRD_PARTY_NOTICES.md)

Most documents are written in Chinese.

## Data and safety

Mod data is stored in `%LOCALAPPDATA%/ITB-SL/P04`. Uninstalling does not delete it and does not restore
game progress. Back up before first use, and do not use the temporary slot as a long-term progress
backup.

## License

Original mod code and documentation are [MIT](LICENSE). Into the Breach and its assets, as well as
ITB-ModLoader, remain the property of their respective owners and are not part of this repository.
