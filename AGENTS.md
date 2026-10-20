# spelunky2-43aspect

native 4:3 mod for spelunky 2, running under proton/FEX on an android handheld.

**read `.agents/system.md` before doing anything.** it has the device layout, the
build + deploy commands, the address reference, and the history of what has
already been ruled out. this file is only the short version.

## device

```sh
ssh -o BatchMode=yes droiddeck '...'     # key auth, lands in the linux container
```

the container **is** the filesystem the game runs from, so deploy, logs, and
`/proc/<pid>/mem` all work over ssh — no adb except for screenshots.

| what | path |
|---|---|
| game dir | `/root/.local/share/Steam/steamapps/common/Spelunky 2/` |
| logs | `<game dir>/mod_43aspect.log` (only when `debug_log = 1`), `mod_43aspect_boot.log`, `spelunky.log` |
| screenshot | `adb -s 6f1b78cc exec-out screencap -p` → 1280x960 |

there is no staging dir or sync any more: the mod is one `discord_game_sdk.dll`
and it goes straight into the game dir (`make deploy`):
`ssh droiddeck 'cat > "<game dir>/discord_game_sdk.dll"' < discord_game_sdk.dll`.
`scp` is broken (no sftp subsystem) — always pipe through ssh like that.
for a device debug run set `debug_log = 1` in the game dir ini (and `ui_2d_dump = 1`
only when you need the dump — it causes the first-pause hitch).

## non-negotiables

- **one issue at a time.** one reversible change per test. keep the known-good
  baseline. verified ≠ predicted. never invent an address or byte pattern.
- **`custom_zoom` stays 13.5.** it is vanilla; `>= 15.0` leaves the camera's
  vertical clamp no valid range and brings back the level-entry jitter.
- **`ui_quads` stays off.** a device run proved it lands with zero visual change
  — those `.rdata` rects are entity/world data, not HUD geometry.
- **`hud_fix` stays off.** never validated.
- **do not trust `aspect_globals = 1`** in the ini; the code ignores it.
- **the user launches the game, not you.** driving steam from ssh leaves the
  client half-updated and unable to start any game (see §12).
- the old dev repo (`~/Downloads/spelunky2-ARM`) is the archive: full commit
  history, run logs, `rollback/` builds, decomps, ghidra project, the
  `llm-experiments` branch (read it, **never merge it**). commit hashes and
  `runN.log` / `rollback/` paths in system.md refer to that repo.

## status

hud, pause menu, camp and world are all 4:3 and user-verified on device (pin11).
the single-dll release (heap fix + 4:3 + audio, debug logging off) is **not yet
verified on device**. native windows pc has never been run. `make test` runs the
host checks; it needs a stock `Spel2.exe` copied into the repo root (gitignored).
