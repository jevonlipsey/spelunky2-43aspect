# Spelunky 2 native 4:3 + Android compatibility

**[Browser patcher (Android)](https://jevonlipsey.github.io/spelunky2-43aspect/)** · **[Download the dll (PC)](https://github.com/jevonlipsey/spelunky2-43aspect/releases/latest)**

A mod for Spelunky 2 that renders the game natively at 4:3 instead of letterboxed 16:9, and fixes the startup hang on Android (Proton via FEX). It also un-stretches the 2D UI for 4:3 and restores the sound that is otherwise missing on Android.

The same mod ships two ways: patched into `Spel2.exe` by a web page, or as a drop-in `discord_game_sdk.dll`. Use one, not both.

## Install

### Android: browser patcher

1. Open https://jevonlipsey.github.io/spelunky2-43aspect/ (or `index.html` locally).
2. Drop your `Spel2.exe` on the page and save the patched file over the original. Keep a backup.
3. Launch the game. `mod_43aspect.ini` is created next to `Spel2.exe` with defaults on first launch.

The page adds the Android heap fix and embeds the 4:3 mod in the exe as a new section that runs before the game starts. Nothing else to install, and Discord rich presence keeps working. The file is processed locally and never uploaded. Steam's "verify integrity of game files" reverts the patch. An exe already patched by GabrielGabriel197's Android patcher is accepted.

> *Only want the Android fix with stock 16:9? Use GabrielGabriel197's patcher: https://github.com/GabrielGabriel197/Spelunky2-Android-Patch*

### Windows PC: drop-in dll

1. Back up the game's original `discord_game_sdk.dll`.
2. Download `discord_game_sdk.dll` from the [latest release](https://github.com/jevonlipsey/spelunky2-43aspect/releases/latest) and copy it into the game folder, next to `Spel2.exe` (for example `steamapps/common/Spelunky 2/`), replacing the original.
3. Launch the game. `mod_43aspect.ini` is created next to `Spel2.exe` with defaults on first launch.

No exe patching is needed (the dll applies the heap fix in memory too, so it also works on Android). Discord rich presence is disabled, because this dll replaces the real Discord SDK. To uninstall, restore the original dll. If the exe was also patched by the page, the dll stays idle.

## Configuration

`mod_43aspect.ini` sits next to `Spel2.exe`. The keys most players care about, with defaults:

| key | default | what |
|---|---|---|
| `heap_patch` | `1` | shrink the allocator's address hint range so it fits Android's 39-bit address space. An exe already patched by the browser tool is tolerated |
| `force_audio` | `1` | re-apply the `local.cfg` sfx / music volumes to the audio buses for the first minute (the game leaves them muted on Android) |
| `custom_zoom` | `13.5` | camera zoom. 13.5 is vanilla. Do not raise it: at 15.0 or more the camera's vertical clamp has no valid range and the camera jitters on level entry |
| `ui_2d_scale` | `1` | un-stretch the 2D UI (HUD icons, timer, menu text) for 4:3. `0` leaves it stretched |
| `debug_log` | `0` | write `mod_43aspect.log` next to the exe. Leave off for normal play; logging on the render thread can cause hitches |

All other keys are tuning knobs and are documented in the comments of the ini.

## Compatibility

- Built for `Spel2.exe` 585,350,144 bytes, sha256 `15a31692700c3c9471d21a0c4cc5ddf74ff2aac965d7c6fff92ff75fe9636e54` (PE timestamp 2022-11-14).
- The mod uses hardcoded addresses for that build and checks the existing bytes before writing. On a different build the patches fail safely (they are skipped and logged) instead of corrupting anything, so the mod simply does nothing useful there.
- Tested on Android only (Retroid Pocket Nova, Proton via FEX in DroidDeck, 1280x960). Native Windows PC is untested.

## Build from source

Needs `x86_64-w64-mingw32-gcc`.

```sh
make release   # builds discord_game_sdk.dll from mod_43aspect.c
make page      # release, then embeds that dll in index.html for the web patcher
make test      # host checks; the patcher test needs a stock Spel2.exe in the repo root
```

## License

MIT, see [LICENSE](LICENSE). Spelunky 2 is © Mossmouth; this project ships no game files and is not affiliated with Mossmouth or Discord.

## Credits

- GabrielGabriel197 for discovering the original Android patch: https://github.com/GabrielGabriel197/Spelunky2-Android-Patch
