# Spelunky 2 native 4:3 + Android compatibility

A mod for Spelunky 2 that renders the game natively at 4:3 instead of letterboxed 16:9, and fixes the startup hang on Android (Proton via FEX). It also un-stretches the 2D UI for 4:3 and restores the sound that is otherwise missing on Android. It ships as a single `discord_game_sdk.dll` that the game loads in place of its Discord SDK.

## Install

### Option A: Full 4:3 + Android Compat

1. Back up the game's original `discord_game_sdk.dll`.
2. Download `discord_game_sdk.dll` from the [Releases](../../releases) page (or build it, see below) and copy it into the game folder, next to `Spel2.exe` (for example `steamapps/common/Spelunky 2/`), replacing the original.
3. Launch the game. `mod_43aspect.ini` is created next to `Spel2.exe` with defaults on first launch.

Notes:

- No exe patching is needed. The dll applies the Android heap patch itself when the game starts.
- Discord integration (rich presence) is disabled, because this dll replaces the real Discord SDK.
- To uninstall, restore the original `discord_game_sdk.dll`.

> *Note: This 4:3 mod includes the Android heap compatibility patch built-in. If you only want the standalone Android fix while retaining stock 16:9, please use GabrielGabriel197's patcher: https://github.com/GabrielGabriel197/Spelunky2-Android-Patch* .

### Option B: Browser EXE Patcher

Applies only the Android compatibility (heap) patch, with the stock 16:9 picture.

1. Open the patcher: `https://<user>.github.io/<repo>/` (TODO: replace with the real GitHub Pages link), or open `index.html` locally.
2. Drag and drop your `Spel2.exe` onto the page and save the patched file over the original.

Notes:

- Processing is local; the file is not uploaded anywhere.
- Keep a backup of the original `Spel2.exe`.
- Steam's "verify integrity of game files" reverts the patch.

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
make test      # host checks; the patcher test needs a stock Spel2.exe in the repo root
```

## Credits

- GabrielGabriel197 for discovering the original Android patch: https://github.com/GabrielGabriel197/Spelunky2-Android-Patch
