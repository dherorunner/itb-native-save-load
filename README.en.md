# Into the Breach: Native Save & Load

**v0.4.7 · by dherorunner · MIT**

[中文](README.md) · [![build](https://github.com/dherorunner/itb-native-save-load/actions/workflows/build.yml/badge.svg)](https://github.com/dherorunner/itb-native-save-load/actions/workflows/build.yml)

An unofficial mod that saves a battle and loads it back from inside the game: press
**Esc → 存档 / 读档 (Save / Load)**. The in-game buttons are Chinese; translations are provided below.
It calls the game's own save/load functions, so no Python, console or external
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

**[Download the v0.4.7 Windows player ZIP](https://github.com/dherorunner/itb-native-save-load/releases/download/v0.4.7/ITB-Native-Save-Load-v0.4.7-windows.zip)** · [Release page](https://github.com/dherorunner/itb-native-save-load/releases/tag/v0.4.7)

Requirements: Windows ITB 1.2.93 with the exact EXE hash under Requirements below, keyboard/mouse,
and ModLoader 2.9.6. No other mod dependency is required.

1. Close the game. Press Win+R, enter `shell:Personal`, then open `My Games/Into the Breach`.
   Copy that **entire folder**, including all `profile_*` folders and settings, to a safe location outside the game directories.
   This also works when Windows Documents is redirected to OneDrive. If you have used the mod before,
   separately back up `%LOCALAPPDATA%/ITB-SL/P04` (open it with Win+R). A missing mod data folder is normal before initialization.
2. If ModLoader is not installed, install
   [ITB-ModLoader 2.9.6](https://github.com/itb-community/ITB-ModLoader/releases/tag/v2.9.6) following the
   [official instructions](https://github.com/itb-community/ITB-ModLoader/tree/v2.9.6#installation).
3. Download [ITB-Native-Save-Load-v0.4.7-windows.zip](https://github.com/dherorunner/itb-native-save-load/releases/download/v0.4.7/ITB-Native-Save-Load-v0.4.7-windows.zip), unzip it and copy the
   **`itb_sl` folder** into the game's `mods` directory, then enable it in ModLoader's mod management
   screen.

In Steam, right-click the game in your library → Manage → Browse local files. The folder containing
`Breach.exe` is the game root. Extract the **loader** into this root, overwriting as its official instructions require;
copy the **mod's `itb_sl` folder** into `mods` (create `mods` if missing). Do not install either ZIP into the save folder.
Apply/restart as requested by ModLoader after enabling the mod.

The result should contain `Into the Breach/mods/itb_sl/scripts/init.lua` and
`Into the Breach/mods/itb_sl/native/itb_sl_p04.dll`, with no extra nesting level.
GitHub's auto-generated "Source code" archives are not installable mods.

To check the game EXE, open the game root in File Explorer, type `powershell` in the address bar and
press Enter. Run `Get-FileHash -Algorithm SHA256 -LiteralPath .\Breach.exe` and compare the Hash column
with Requirements below (case does not matter). A mismatched EXE is unsupported.

**Initial installation check:** enter a profile and battle, then open **Esc → 存档 / 读档**.
The panel should show **战前自动档** (auto save) and **临时手动槽** (manual slot).
An empty slot is normal initially. Opening the panel confirms the entry loads; it does not verify a save/load round trip.

## First save and load

Keep the game running and stay in the same battle. Make a full backup before trying this mod.

| Chinese button / status | Meaning |
|---|---|
| 保存到临时手动槽 | Save to the temporary manual slot |
| 读取临时手动槽 | Load the manual slot |
| 读取战前档 | Load the pre-deployment auto save |
| 关闭 | Close the panel |
| 可用 / 空 | Available / Empty |
| 本次进程外 / 其他战斗 | From an earlier game process / Another battle; cannot load |

1. Finish deployment and wait for a stable player turn with no animation. Open the panel and choose
   **保存到临时手动槽**. Confirm overwriting if a manual save already exists.
2. Wait for processing to finish. Reopen the panel to check **已保存** (Saved) and a manual slot marked **可用** (Available).
3. Close the panel and try your moves. To return, reopen the panel, choose **读取临时手动槽** and confirm.
   **Loading discards actions made since that save.** Canceling the confirmation does not load.
4. Wait for completion. The panel can show **已读取，可继续操作** (Loaded; ready to continue).
   Check the board and mech positions before continuing.

For an **auto save**, enter a new ordinary battle normally from the map with the mod enabled.
The mod attempts capture during a stable pre-deployment window. Check that the auto slot is **可用**;
after deploying or acting in that battle, use **读取战前档** to return to deployment.
Continuing an already-started battle does not create a missing auto save. Manual saving/loading does not replace
an already-captured auto save for that battle; the next successful new-battle capture replaces it.

Only Available slots can load. Saving is blocked during deployment, enemy turns, animations or other dialogs;
the disabled appearance can be unclear, so wait for a stable player turn and reopen the panel.
Restarting the game or entering another battle makes old slots unreadable, even if files remain on disk.
Use the game's normal Continue for persistent progress; these temporary slots are not long-term backups.

## Upgrade, uninstall and recover

Close the game and back up both data folders first. For an upgrade, move the old `mods/itb_sl` outside
`mods`, then copy in the complete new folder. Do not mix DLLs/scripts or keep a renamed old mod inside `mods`.
For uninstalling this mod, disable it and restart, or move `mods/itb_sl` outside the game directory while the game is closed.
Keep the data folders; uninstalling does not restore progress or make old slots readable.
Uninstalling this mod does not require removing ModLoader; to remove the loader, follow its
[official uninstall instructions](https://github.com/itb-community/ITB-ModLoader/tree/v2.9.6#uninstallation).

To restore progress, close the game, preserve the current user data folder elsewhere, then copy your **complete matching backup**
back to its original location. This discards progress made after that backup. Do not restore saves over the game installation;
the resulting path should be `My Games/Into the Breach/profile_*`, with no extra nesting.
If Steam reports a cloud conflict, preserve copies and confirm their timestamps before choosing which to keep.

## Troubleshooting and feedback

- **No menu entry:** check ModLoader/mod enablement and folder nesting, then enter a profile and battle.
- **Unsupported version:** compare the EXE hash; do not bypass the check.
- **Native service fails to load:** reinstall the complete mod folder with the game closed and check whether security software quarantined its DLL.
  Do not disable protection or obtain replacement DLLs from unknown sources.
- **Save/load failure or service disabled for this process:** stop retrying, preserve the current state and record the full message.
  If copying files for evidence while the game is running, treat the copy as an incident snapshot, not a verified consistent backup.

Report issues through [GitHub Issues](https://github.com/dherorunner/itb-native-save-load/issues). Include Mod/ModLoader/game/Windows versions,
EXE hash, other mods, exact steps, expected/actual behavior and error text, and whether the game was restarted or the battle changed.
Review screenshots/logs for personal paths or game data before sharing; do not upload whole save or backup folders.
The [detailed limitations](KNOWN_ISSUES.zh-CN.md) are in Chinese: other tasks/mod combinations, resolutions, special terrain/units and all unit fields
are not fully validated; profile internal IDs must be 1–32 ASCII letters, digits, underscores or hyphens.

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

Run that command from the repository root (the folder containing `release.json` and `tools`). Edit the
individual Lua modules in `src/mod/scripts`, not the generated `scripts/init.lua` in a build package.
Review the diff, build once, and state clearly whether validation was offline or in game when opening a pull request.
CI builds and checks package files; it does not run the game or prove gameplay correctness.

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
