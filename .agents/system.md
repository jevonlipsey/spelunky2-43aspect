# spelunky 2 native 4:3 — workspace + device context

everything an agent needs before touching this repo. written so the device, the
build, and the history of the aspect work never have to be re-explained.

---

## 1. what this is

`discord_game_sdk.dll` (built from `mod_43aspect.c`) is a freestanding x86_64
windows dll that the game loads at startup in place of its real discord sdk, and
it patches `Spel2.exe` to render **native 4:3 at 1280x960** on the user's
handheld (Retroid Pocket Nova, Android 13, aarch64). the game is a windows
binary running under **Proton** (`DroidDeck-Proton 11-7.5`) via **FEX-Emu**
(x86_64 → arm64), inside a **proot** linux container managed by the
**droiddeck launcher** android app.

injection is that single dll. there is no shim and no separate mod dll any more
(the old two-binary setup is in the archive repo's `legacy/`). it also applies the android heap
patch itself, so the exe needs no patching. it exports a `DiscordCreate` stub
that fails, so discord rich presence is off.

### status
- **works** (user-verified, pin11): 4:3 world, camera (jitter fixed), hud,
  pause menu, character select / camp, rope, transitions. section 8 is how the
  2d ui got there.
- **not yet on device**: the single-dll release (heap patch + audio port +
  debug off, which is also the first-pause lag fix). pc never run.

---

## 2. repo layout

| path | what |
|---|---|
| `mod_43aspect.c` | the mod. one file, freestanding (no CRT) |
| `mod_43aspect.ini` | runtime tuning, lives next to `Spel2.exe`; created with defaults on first launch |
| `discord_game_sdk.dll` | build output: the whole mod, loaded as the game's discord sdk |
| `index.html` | client-side browser patcher, heap patch only (stock 16:9) |
| `README.md` | public readme for players |
| `tools/test_patcher.mjs` | node test for the browser patcher |
| `tools/gen_alg.sh` + `tools/test_alg.c` | host harness: runs the shipped ui_2d code on recorded cases (`make test`) |
| `Makefile` | build + deploy targets (section 4) |
| `tools/snap.py` | read the live game out of `/proc/<pid>/mem` (section 7) |
| `tools/ddlaunch.py` | run something with a desktop process's env over ssh |
| `tools/fx.py` | static-analysis helper: `rip_refs`, `func_for`, `read_float`, section map |
| `Spel2.exe` | **not tracked** (gitignored). copy a stock one into the repo root for `tools/fx.py` and `make test` |

### history
this repo started clean from the dev repo `~/Downloads/spelunky2-ARM` at
`f9d85ed`. every commit hash, `runN.log`, `rollback/`, `legacy/`, `decomp/`
and the `llm-experiments` / `fix-43-bugs` branches below live **there**.

---

## 3. device access

```sh
ssh -o BatchMode=yes droiddeck '...'
```

`droiddeck` is a local ssh alias (key auth, no password) that lands you in the
**linux container** on the handheld — the same filesystem the game runs from.

- `uname -a` reports the *android* kernel; that is expected.
- `id` shows `uid=10114(root)` — the launcher app's uid. everything in the
  container is owned by it, so `/proc/<pid>/mem` on the game is readable.
- `adb -s 6f1b78cc` also works over usb, and is only needed for screenshots.
- handheld LAN ip: `10.6.6.189`.
- the linux desktop is forced to **1280x960** (`BL_WIDTH`/`BL_HEIGHT`); `adb
  exec-out screencap -p` also returns 1280x960, so screenshot pixels map 1:1 to
  the coordinate space the game uses.

### paths that matter

| what | path (in the container) |
|---|---|
| game dir | `/root/.local/share/Steam/steamapps/common/Spelunky 2/` |
| same thing as wine sees it | `S:\steamapps\common\Spelunky 2\` |
| android downloads (legacy mod staging, now unused) | `/root/Storage/Download/` |
| same thing as android sees it | `/sdcard/Download/` (what `adb push` writes) |
| steam root | `/root/.local/share/Steam` |
| session request dir | `/data/user/0/com.droiddeck.launcher/files/session` |
| session logs | `/data/user/0/com.droiddeck.launcher/files/logs/<date>-<mode>/` |
| ssh home | `/root` |

**`/sdcard` on the linux side is not android's `/sdcard`.** android's storage is
bound to `/root/Storage`, so `/root/Storage/Download` **is** `/sdcard/Download`.
pushing either way lands in the same file.

### there is no sync rule any more

the shim used to `CopyFileA` the mod from the download dir over the game dir at
every launch. that is gone: the dll in the game dir is the mod, and what you
write there stays. old copies are inert: `/root/Storage/Download/mod_43aspect.dll`
+ `.ini`, and a stale `mod_43aspect.dll` / `mod_probe.dll` in the game dir, are
never loaded by anything.

---

## 4. build + deploy

```sh
make release      # default; strips symbols (-s)
make debug        # unstripped, for a usable backtrace — but see the warning
```

- **`-s` is required to deploy.** the game's loader rejects the unstripped dll
  (~102 KB of dwarf).
- `the mod is built with `-nostdlib -fno-builtin -mno-stack-arg-probe -Wl,-e,DllMain`.
  there is no CRT: use the in-file `my_memcpy` / `my_format`, not `memcpy` / `sprintf`.

### deploying without adb (preferred)

`scp` does **not** work — the device sshd has no sftp subsystem. pipe stdin:

```sh
make deploy    # = ssh droiddeck 'cat > "<game dir>/discord_game_sdk.dll"' < discord_game_sdk.dll
```

`make deploy` builds a release first and pushes the dll only. it deliberately
does **not** push the ini — it holds hand-tuned values, so pushing it every
build silently reverts them. `make push_ini` / `make pull_ini` are the explicit
opt-ins. `make test` runs the host checks.

for a debug run set `debug_log = 1` in the device ini (`ui_2d_dump = 1` only
when you need the dump).

---

## 5. reading logs

```sh
GD='/root/.local/share/Steam/steamapps/common/Spelunky 2'
ssh droiddeck "cat \"$GD/mod_43aspect.log\""
```

| file | contents |
|---|---|
| `mod_43aspect.log` | only written when `debug_log = 1` (default 0). the mod's report: the toggle line, one `[ OK ]`/`[FAIL]` per patch, `[matrix] poll N` lines, `[watch]` heartbeat |
| `mod_43aspect_boot.log` | dllmain-time crumbs only. smaller, so it survives a crash during init |
| `spelunky.log` | **the game's own log.** tells you how far init actually got |

the toggle line is the fastest way to confirm what actually got applied:

```
[*] ar=1.33333 world=1 no_lb=1 cam=1 quads=0(x1.00000 y1.00000) hud=0(x1.00000) (gl=1 pc=0 vec=0 ri=0) zoom=13.50 dbg=1
```

a good run ends at `[+] SUCCESS: all patches applied and verified`, then
`[matrix] poll N: ... [ 4:3 OK ]`.

---

## 6. screenshots

```sh
adb -s 6f1b78cc exec-out screencap -p > shot.png   # 1280x960 png
```

this captures whatever the android display shows, which is the game. it is the
only way to *see* the result — there is no live screen protocol.

---

## 7. reading the live process (`tools/snap.py`)

installed on the device:

```sh
ssh droiddeck 'cat > /root/snap.py' < tools/snap.py
ssh droiddeck 'python3 /root/snap.py'
```

while `Spel2.exe` runs this prints: the module base, an `MZ` check on it, the
baked 16:9 constants (a base sanity check), and the **2d canvas geometry** —
which is the one number that has never been observed at runtime.

why this matters: adding a log line costs a rebuild + push + relaunch. reading
memory costs nothing and can answer questions nobody thought to log. the guest
is x86_64 under FEX, which maps guest vas through, so a guest address from the
mod's own log (`[*] base=0x...`) is readable here at the same value.

`/proc/<pid>/mem` is also *writable* in principle (we own the uid), but `.text`
is read-only and FEX caches translations, so runtime code patching is not
reliable. change the mod and relaunch instead.

---

## 8. the open problem: skinny HUD / menu text

### symptom
measured from a device screenshot (2000x1536, pre-fix): uniform ~0.75
horizontal compression. bomb ≈ 0.62, rope ≈ 0.73, heart ≈ 0.86 wide-per-tall.
the heart only looks better because its source sprite is not square. pause-menu
text is skinny the same way.

### root cause (established)
the game's UI geometry is **authored against a 16:9 frame**: quads are baked at
`w/h = k × 0.5625`, and elements are built with runtime `× 1.7778`. the mod
forces the canvas to 4:3 (1280x960) so the world fills the screen, which leaves
the whole 16:9-authored layer 0.75 narrow in 4:3 ndc space.

the aspect is baked **per element**, so no single float rescales the geometry.
but the geometry is not where you have to fix it — see the next section.

### the single bottleneck (found)
every 2d ui element — hud icons, timer, pause-menu art, every glyph — is emitted
into one pooled vertex batch and drawn by **one function**:

```
0x22cc5d80  the 2d batch flush/draw   69 direct callers in .text
```

`0x22c5d870` ("Drawing HUD elements") builds the hud's batch and calls it. this
is the real draw, and it is *before* the two stores that `95818d0` hooked:

```
0x22c5e12d  call 0x22cc5d80          ; <- the hud draw itself
0x22c5e132  mov  byte [rsi+0x802ad], 0x37
0x22c5e139  mov  dword [rsi+0x80318], 0
0x22c5e143  mov  rcx, [0x22e18c68]   ; cached display struct
0x22c5e14a  mov  rax, [rcx]
0x22c5e190  jmp  qword ptr [rax+0x1b8]   ; tail call, not a ret
```

the pending batch, read out of `0x22cc5d80` and the emitters:

```
+0x802f0 vb wrapper    +0x802f8 vb write ptr   +0x80300 vb room left
+0x80308 ib wrapper    +0x80310 ib write ptr   +0x80318 pending indices
+0x8031c pending verts +0x802ad render state   +0x802ac state already bound
```

the flush binds shader/blend/sampler, draws `[+0x80318]` indices, then **releases
the wrappers and zeroes `+0x802f0..+0x802ff` and `+0x80308..+0x8031f`** (two
`movups` at `0x22cc5f79` / `0x22cc5f7d`), so `+0x8031c` is always *this* batch's
vertex count and the batch is exactly

```
[ *(u64*)(disp+0x802f8) - n*0x18 , *(u64*)(disp+0x802f8) )
```

0x18 per vertex — the emitters (`0x22b91700`..`0x22b94d30`, `0x22cc6850`) write
`x,y,z,u,v,argb` and z is always 0. a 0x24-byte variant exists for other render
states; the input layout picks it via `[+0x80560]`/`[+0x804f8]` and it is left
alone.

`0x22e18c68` is a lazily-cached `[[0x22e1e798]+0x10]`, i.e. the same display
struct the renderer uses. the viewport in effect for that flush is the one set
at `0x22cbfee9` from the **2d canvas** (`disp+8` / `disp+0xc`) — set once per
frame, covers the world and the ui both, no per-pass viewport switch anywhere
between `0x22cbfee9` and `0x22cc192b`.

### what does NOT work (don't re-derive these)
- **`ui_quads` — definitively not it, and it stays off.** a device run proved
  the patch lands (probe read `w/h=0.75000`, was `0.5625`) with **zero visual
  change to the HUD**. the 98 baked rects in `.rdata`
  `0x22d0d000`–`0x22d12000` are **entity/world** data: they are consumed by
  entity init at `0x22aa4800`, which copies `0x22d0df30`/`0x22d0df40` into
  `[rax+0x60]`/`[rax+0x70]`, and the same 12 KB holds the world/camera aspect
  vectors. a full `.rdata` scan found **no second baked-quad table**: 102
  structural hits, 98 inside that range, the other 4 are false positives, and
  every one of the 98 is `k × 0.5625`.
- **the llm's `hud_icon_width` theory** (`0x22d0a764` `0.225` → `0.3`) — wrong
  for the same reason. that `0.225` has ~200 refs across entity code and
  patching it would be catastrophic. a neighbouring `0.225` at `0x22c5e0d0` is
  an animation-frame blend weight (`16 × (x − 0.5)`).
- **`hud_fix`** — a separate mechanism that was never validated. off.

### why `ui_letterbox` (95818d0) did not work — established

the hook was placed at `0x22c5e132`, i.e. **after** `0x22c5e12d`. the hud was
already drawn when the band viewport was installed, so:

- the icons never saw the band → still stretched in level;
- the band was never taken back down → every later pass (menus, loading screen,
  the pause dim) drew into it → the 16:9 letterbox the run showed.

both symptoms, one wrong instruction boundary.

### the fix in this build

**`ui_2d_scale` (default 1)** — un-stretch the 2d layer where it is drawn. the
entry of `0x22cc5d80` is hooked; its 14-byte prologue is replaced by 12 bytes:

```
48 b8 <imm64 &ui_2d_flush_hook>   mov rax, <mod function>
ff e0                             jmp rax
```

`ui_2d_flush_hook()` scales y of the pending batch (`[+0x802f8] − n*0x18`,
n = `[+0x8031c]`) by `(cw*9/16)/ch` = **0.75** at 1280x960, leaving any
**element that covers the frame and touches an edge** alone — a y box at least
`UI_2D_FILL_SPAN` (1.8) tall reaching `±(ui_2d_edge−0.005)`: those are the
full-screen fade / dim / blur fills, which must keep covering the window.
art that merely overshoots the frame (a camp column with its top at `y 1.3`
while it zooms) scales like everything else: per-vertex skipping used to shear
such a quad where it crossed ±0.995 and left its top slice 16:9 tall, which is
the "16:9 background flashes then scales" report. `ui_2d_fill = 0` restores
that per-vertex line (and the shearing) if a fill ever disagrees with the test.
**vertices further than `±UI_2D_NDC_Y` (8.0) are never touched** — the same
flush carries the world's 2d geometry with y in the tens. x is left alone (ndc
x spans the same 1280 px either way); `ui_2d_scale_x` exists if the device run
says otherwise. then it replays the 14 prologue bytes and jumps to the body at
`0x22cc5d8e`, so the game's own epilogue and every return path still run.

the trampoline is a top-level `__asm__` block (`ui_flush_body_enter`) and the
resume address is `ui_flush_body_va`, written by `patch_ui_2d()`.

**`ui_letterbox` (default 0, now fixed)** — the alternative, and now placed
where it belongs: 22 bytes at `0x22c5e12d` (`call 0x22cc5d80` + the two stores)
are replaced with `mov rax,<stub>; call rax; 10 nops`. the stub sets the centred
16:9 band, calls the real draw (`ui_flush_body_enter`), **restores the canvas
viewport**, and only then does the two displaced stores. never enable both —
they fix the same axis from opposite ends and would double up.

**`ui_2d_pin` / `ui_2d_pinspan` / `ui_2d_cluster` (defaults 0.85 / 0.35 / 0.20)**
— the first `ui_2d` device run proved the proportions ("the ui actually FINALLY
looks square") but not the position. scaling about the screen centre drags
everything that was authored near an edge into the middle, so the in-level hud
bar landed 120 px low, and the CHOOSE ADVENTURER banner / Quick Select went with
it.

fix: the batch is walked as quads (4 vertices each); consecutive quads whose y
centres sit within `ui_2d_cluster` of each other are one element (a text run, a
row of icons, one nine-slice panel). a batch is pinned **only when every one of
its elements** is compact (`<= ui_2d_pinspan` tall) and past `ui_2d_pin` from
the centre — i.e. it is pure hud. then each element scales about the screen edge
it hugs, `y' = a + (y - a) * 0.75` with `a = ±1`, which reproduces vanilla's
**pixel** distance from that edge: a hud vertex at `y = 0.94` becomes `0.955` =
21.6 px under the top of the 960 window, exactly the 21.6 px it sits under the
top of the 720 frame.

everything else — a batch with even one non-edge element — is **artwork or a
mixed pass**, and every element in it scales about **its own centre**, which
does not move a single authored position (only heights get 0.75x). this matters
because the choose-adventurer camp is one stack of such batches (quads from
`y 0.06` to `0.94` at `x -0.51..-0.23`, several clusters each, some of them
past any pin line); pinning just the edge-ish clusters inside them tore the
picture in two — the "quads and text still fucked" of run 4.

threshold 0.85 is picked from measurements, not taste: the hud elements'
authored means sit at 0.86–0.92 (icon `0.86..0.94` pinned reads `0.895..0.955`
on screen), so they pin exactly as approved; the banner text measures
`y 0.706..0.781` (mean 0.743) in `a50fe75` — which has **no** 2d scaling, so
those are authored coordinates — and can never pin. clusters in the
choose-adventurer art span 0.62..0.94, so **no threshold alone** can separate
them from the hud; that is why the decision is per-batch.

**run 5 refinements (the three glitches the guest reported).**

- **rope break** — three n=4 rope segments at `x -0.022..-0.006` (dead centre),
  y means 0.957 / 0.875 / 0.859, are compact and past 0.85, so they pinned and
  jumped 5–17 px up while the segments below them (y < 0.85) scaled in place:
  the rope visibly broke. fix: an element may also only pin when its **x centre
  is past `ui_2d_pinx` (default 0.30)** — hud lives in the corners, the rope
  and the scroll banner sit centred. all run-5 pinned clusters (`x 0.45..1.01`,
  `0.78..0.93`) keep pinning; the only ones lost are the rope's.
- **pause/dialog text sits low** — shape 100 (run 5, `b 6131`) is the whole
  pause text block: title row at ~0.36 plus five item rows on a 0.135 pitch,
  78 quads, ONE cluster under the centre rule (`ui_2d_cluster` 0.20 > the
  0.135 row pitch). scaling that block about its mean (−0.070) squeezed the
  rows towards the middle: the title dropped ~51 px onto the banner, the outer
  items moved ~30 px — "the pause menu text is shifted down a bit too much…
  same across all the menus". fix: a quad whose y box is further than
  **`ui_2d_rowgap` (default 0.05)** from the previous quad's box starts a new
  element even when the centres are close, so stacked text rows keep their
  authored row positions and only their heights shrink. tiling pieces
  (nine-slice strips, roof tile bands, sprite columns) touch and never split;
  the rope segments (0.013 tile) stay whole as before.
- **16:9 flash on the zoom transition** — see the ui_2d_edge paragraph above:
  overshooting art now scales consistently instead of being skipped beyond
  ±0.995 while the rest of it compressed.

**run 6: the bands — the flush carries the world, and nothing was guarding it.**

the build above (all three fixes) was rejected on device: black bands across
the level, level-wide, visible in-level and behind the pause menu. `ee60541`
was clean. the dump never shows it, because `ui_2d_dump_shape` filters
non-ndc batches itself (`ymin < -8 || ymax > 8`) — but `ui_2d_apply_runs`
did not, and **it never had to**: `ee60541`'s per-vertex line
`if (y < -ui_2d_edge || y > ui_2d_edge) continue;` skipped every world vertex
too, because world y is in the tens, far outside ±0.995. the per-vertex line
was doing two jobs: leaving the fills alone *and* leaving the world alone.
replacing it with a per-element fill test removed the world's only guard, so
each world cluster got scaled 0.75 about its own mean — every room row
compressed toward its own centre, and the gaps between rows became the bands.
(the dump's own comment already said the world rides this flush with `y ≈100`,
`z 5..40` — the filter hid the fact that the scale path has no filter at all.)

the guards are now explicit and separate: `UI_2D_NDC_Y` (8.0) stops any
vertex in the tens from being scaled, and the fill test is per element
(`UI_2D_FILL_SPAN` 1.8 + touches an edge). `ui_2d_fill = 0` still restores
ee60541's per-vertex line, world guard included, if a fill ever disagrees.

host harness cases added: a world batch at `y 99..103` scales **0** verts and
is byte-identical (both fill modes); the run-6 shape 14 cover quad
(`x -1..0.067, y -1..0.896` — it rests on `y = -1`, so scaling it lifted its
bottom edge 114 px and opened a band) now scales 0; the overshoot column still
scales 4 of 4; with `ui_2d_fill = 0` it scales 2 of 4.

the dump's x/y ranges, quad centres and cluster means/spans are **authored**
values (the dump runs before the scaling is applied), which is why run-5 shape
100 reads `y -0.379..0.384` while its rendered position is 0.75x around its
mean. the dump prints at most 8 clusters per shape (run 5's `#100` printed
1, it will print ~6 after the row split).

`ui_2d_dump` (default 1) logs one line per distinct batch shape — `n`, `idx`,
the x/y/z range, the first 12 quad centres and the per-cluster decision (mean,
span, anchor) — so a single device run lists every pass that exists without
per-frame spam. turn it off once the layout questions are answered. it ignores
non-ndc batches: **the world rides through the same flush** with `y ≈ 100` and
`z = 5..40`, and in the first dump run it filled every slot before the ui was
ever reached. note the table caps at 400 shapes now (was 160, which hid the
camp rope entirely) and it dedups on `n / nidx / y-buckets` only, so a full
table hides later passes (the visible hud never got logged in run 4 for
exactly that reason). it also logs every **canvas change** (`[2d] canvas W x H
sy ... batches N`) and, for rope-like columns and big backdrops, the first 24
quad boxes (`[2d] q cy h w ...`) plus the first vertex's alpha (`[2d] a`).

**run 7: the row split was cutting art, and only half-cutting text.**

`ui-2d-pin5` on device: the bands were gone, the hud and the camp art were
good, and the rope stopped breaking — but two new reads came back. (1) the
pause/confirm text rows were *uneven*: shape 80 (n 312, 78 quads = ~13 letters
a row) split into **three** clusters — title `0.350/0.069`, one item
`0.213/0.058`, then items 2-5 chained into one `-0.150/0.486` — so the rows
landed at 0.135, 0.192, then 0.101 pitch against their authored 0.135; the
first items spread, the lower ones squeezed. the old rule compared the *next*
quad's box against the *previous quad's* box, so a chain that survived one
row transition kept merging until the 0.20 centre rule caught it. (2) the rope
showed bands and the camp gate pieces sat wrong: the same rule was cutting
tiling strips the per-vertex line never touched (rope pieces are 0.016 wide
segments 0.03 tall at 0.06 pitch — every gap read as a band).

the rule is now text-only and order-independent: a candidate quad is compared
against the **whole run's box so far**, and may only start a new element when
it looks like a glyph (`UI_2D_ROW_H` 0.12 tall, `UI_2D_ROW_W` 0.06 wide), the
run so far is at least `UI_2D_ROW_RUN` 0.06 wide, and the run is at least
`UI_2D_ROW_N` 6 quads long. letters pass all three; camp art pieces are wider
than 0.06, the rope is narrower than 0.06, and a stack of a few small sprites
is shorter than 6 quads — so no art is ever cut. host cases: the 6-row letter
block now keeps every authored row centre (0.36/0.22/0.085/-0.06/-0.20/-0.34
all unmoved, heights 0.75x), the 8-segment rope strip stays one element (its
segments shrink together, gaps close by 0.25x instead of opening), wide-quad
rows stay one block, and every old case (hud pins, nine-slice, fills, world
guard, overshoot) is unchanged.

**run 8: the groups themselves were wrong for letters drawn as triangles.**

`ui-2d-pin6` came back "no difference, same exact issues", and the log says why:
the pause text (`shape 78`, `n 312 idx 312`) still split into three clusters
(`0.350/0.069`, `0.213/0.058`, `-0.150/0.486`). **`idx == n` means the batch is
not indexed quads at all — it is a triangle list**, so the text is 52 quads of
*6* vertices (2 triangles each), not 78 quads of 4. every helper grouped 4
vertices at a time, which straddles the letters: a group holding the tail of one
row and the head of the next has a box that bridges the gap between them, so the
row split could never fire however the rule was written. that also explains why
only *some* boundaries ever split: a row whose letter count is even lands on a
group boundary by luck.

the group size is now derived from the batch: `n * 6 / idx` when `idx % 6 == 0`
(the vertices per quad either way — 4 for indexed quads, 6 for a triangle list),
clamped to 3..6, else 4. host proof, a 6-row block of 9 letters (odd, like
"Options" 7 / "Main Menu" 9): with vpq 4 the lines merge into three clusters
(`0.290 0.010 -0.270`, the device's exact signature) and with vpq 6 all six keep
their authored centres (0.36/0.22/0.08/-0.06/-0.20/-0.34, heights 0.75x). every
other host case is byte-identical, because every other batch in the log is
`idx = 1.5n` (4-vertex quads) and still groups by 4.

**the rope is a column of one-quad batches, so no cluster rule can hold it.**

the same log: the camp rope is `n 4 idx 6` segments 0.033 tall at 0.049 pitch
(`x -0.847..-0.829`, segments at y means 0.959 0.853 0.756 0.707 0.609 ... 0.090),
each one its **own batch** — other draws come between them, so "the previous
batch" is never the segment above. scaling each about its own centre opens the
authored 0.016 gaps into bands and moves the pinned top segments (+7 to +19 px)
while the rest stay put, which is the visible break.

a narrow glyph-sized **cluster** now looks for its column: twelve chain slots
keyed by **exact** x (`±0.005`, the segments of a rope share their column to the
digit) and by y (`-0.005 .. +0.30`, i.e. this quad sits below that one). a match
reuses that column's anchor, so the whole column scales about one point; the
pinned top segment gives the column `+1` and a hanging rope then stays on the top
edge with its gaps closing 0.75x instead of opening. hud digits are the same
shape with no column (their x differs per glyph), so they keep their own
anchors; a wide art quad drawn between two segments does not touch the slots and
does not break the chain. host cases: 8 single-batch segments take the top anchor
one after another (gaps 0.030 -> 0.0225), the wide quad in between is untouched,
the next segment still matches, and two digits at x 0.951 / 0.968 keep their own
centres.

**run 9: the chain test has to be per cluster, and the camp needs more slots.**
the pin7 build was still banded, and the log says why. the camp flushes **both**
banner ropes in one batch (`n 8`, e.g. `x -0.914..0.344` with clusters at
`0.117/0.036` and `0.976/0.027` — the two ropes hang from opposite ends of the
banner, so the two segments in one flush are 0.86 apart and `ui_2d_cluster` keeps
them separate), so pin7's batch-wide `ng == 1` test never fired for a single one
of those segments. the camp also draws more than four columns (both ropes at
their own x, the ladder, hud glyphs), so four slots evicted a column before its
next segment arrived. both are fixed: the test is per cluster, the slots are 12.
and the dump's `g` line prints the anchor `ui_2d_apply_runs` is about to use,
chain lookup included — the old line printed `pin ? ±1 : mean`, which made a
working chain and a broken one look **identical** in the log and cost a run.

**the row split's run length is 4, not 6.** run 9's pause text (shape 108, `n 312
idx 312`, vpq 6) came back as five clusters: rows 2-5 each kept their authored
centre at the 0.14 pitch, but the title was still welded to "Resume Game",
because the title is **four** letters and `UI_2D_ROW_N` was 6. four is the
smallest run that is still a line rather than a stack: a rope's segments land in
separate batches (a one-quad batch can never split), and the ping-pong between
`UI_2D_ROW_H` / `UI_2D_ROW_W` / `UI_2D_ROW_RUN` already keeps art, tiles and
strips out.

**run 10 (the pin8 log, `run10_pin8.log`).** two findings, mechanism verified
from the log, visible effect of B still predicted.

- **A: the rope tear is the batch pin overriding the chain.** the rope column
  `x 0.397..0.420` (shapes 361-372) chains to anchor 0.998 down to y -0.79, but
  segments below -0.85 (shapes 368-372, `pin 1`) get anchor -1.000: a lone
  segment past -0.85 with |x centre| > 0.30 passes the hud pin test, and
  `} else if (!pins)` let the batch pin win over the column. -0.790 lands at
  -0.343, the next segment (-0.876) at -0.907 instead of -0.4075: a ~270 px tear
  that recurs whenever the tip crosses -0.85. column `x -1.034` pins its top at +1
  so chain and pin agree there. **fixed in pin9**: a segment that found its column
  always takes the column's anchor (`else`), and the dump's `g` loop drops `!pins`
  so it still prints what apply uses. host T1 shows -0.876 -> -0.907 with the old
  rule and -0.4075 with the fix.
- **B: pause / confirm text rows are fine, the row backing is not.** shape 78
  (pause) and 89 (confirm) split into one cluster per row, each anchored at its own
  centre. bbox centre vs mean is a no-op there (|bbox-mean| <= 0.0055, <= 0.7 px
  after the 0.25 factor) and on "choose adventurer" (shape 222, a degenerate space
  quad at 0.844) bbox is *worse* than the mean - do not switch anchors to bbox. the
  real mismatch is the backing batches, **shape 79** (pause, `n 96 idx 144`) and
  **shape 90** (confirm): quads are 0.24 tall with centres in mixed pairs
  (`0.207 0.350 0.207 0.350 | 0.067 0.210 ...`), the 0.20 centre rule welds each
  pair and the shared mean (0.279, 0.139, ...) moves each bar about 0.018 ndc
  (~8.6 px) while its text row stays put. the camp art (shapes 11-76) has similar
  centre jumps and the dump had no per-quad heights to rule it out, so the rule
  ships **gated**: `ui_2d_bars` (default 0) breaks a cluster between two equal
  height (+-0.01) quads whose centres differ by >= 0.05 and <= 0.75 h. run 10
  dumps `bb` (per-cluster bbox centre) and `q` boxes for any multi-level batch
  (own cap 24) to decide whether it is safe to turn on.

**run 10 result (pin9 on device, `run10.log`).** neither visible bug moved. both
are explained from the log + the user's screenshots.

- **rope: one batch, cut by the 0.20 centre rule.** the CHOOSE ADVENTURER centre
  rope is a single batch (shape 272, `b 25073`, `n 36 idx 54`, vpq 4): 9 quads,
  x -0.063..0.063, each 0.2226 tall, centres -0.890 -0.668 -0.445 -0.223 0.000
  0.223 0.445 0.668 0.890, tiling exactly -1.001..1.001. the pitch (0.223) is
  above `g_ui_2d_cluster` (0.20), so `ui_2d_cluster` broke between every segment,
  each scaled 0.75 about its own centre and 25% gaps opened (the screenshot).
  earlier frames of the same rope (shapes 243/259/263, pitch 0.147/0.169/0.195)
  stay one cluster and were fine. pin9's chain-over-pin fix was correct but it
  targeted the off-centre column (x 0.397..0.420), not the visible one.
- **pause / confirm: text keeps authored positions, boards shrink.** menu text
  and buttons anchor on their own centres (unmoved) while the board / paper art
  shrinks 0.75 about its centre, so lower items hang off the board: pause "Exit
  Game" (row -0.344) sits ~41 px below where a 16:9 letterbox would put it
  (-0.258); exit-confirm "No" (shape 534, y -0.405..-0.340, x +-0.031) ~45 px low;
  confirm body text (shape 532, `n 480 idx 480`, x +-0.388, y 0.107..0.390, one
  cluster, anchor mean 0.270) sits high. letterbox truth = every element of a
  centred overlay scaled about y = 0.

**pin10 changes.** (1) `ui_2d_cluster` merges a quad that continues a vertical
strip (same x box +-0.005 as the run, y gap <= 0.005) whatever its centre
distance; the row split and bars tests still apply after it. the merged 9-segment
rope spans 2.002 and touches the edges, so the fill exemption leaves it unscaled:
a continuous rope at its authored extents - intended. (2) `ui_2d_center` (code
default 0, staged 1): a non-pinned cluster with |x centre| <= 0.02 anchors at
y = 0 (fills still win). anchor logic lives in one function, `ui_2d_anchor`,
used by apply, the dump `g` line and the trace. (3) frame trace: ring of the last
96 flushed ndc batches (b, n, idx, vpq, x/y box, first alpha, anchor of the
first cluster); a vpq-6 batch with n >= 300 and |xmin+xmax| <= 0.04 (max 4, never
the same n twice in a row) logs the ring as `[2d] tr` lines, then the next 24
batches, then `[2d] tr end`. host: T9 rope strip, T10 strip guard, T11 centre
toggle; T1-T7 outputs byte-identical to pin9 (rec hashes compared, mutation check
with the strip rule removed fails T9 and T10).

### why the first pin looked half-applied (run 2)

**text batches are not multiples of 4 vertices.** the hud heart is one quad
(`n = 4`), but the coins / timer / level readouts come through the same flush as
`n = 6`, `n = 18` and `n = 30` — the same quads, emitted as two triangles each.
the `n % 4 == 0` guard in the first pin therefore fell back to the plain centred
scale for every text batch, so the text stayed in the band while its icons
jumped to the top: exactly the "hud elements went to the top, but the text
didn't" report. the same split explains the main menu's description bar, and the
choose-adventurer banner appearing twice — one copy of the element pinned, the
other left in the band.

the walk now groups vertices `vpq` at a time (4 for indexed quads, 6 for a
triangle list — the number is derived per batch, see "run 8" above) and lets the
tail group of a batch take whatever is left, so `n` can be anything. verified on
the host against the real functions for `n = 4 / 6 / 18 / 30` (all pinned, text
included), a tall panel (not pinned), mid-screen items (centred scale only), a
full-screen dim (left at ±1) and a mixed batch (hud row pinned, centred quad in
the same batch untouched).

the 2d-scale build is `307c9bc` + these two mechanisms, 27136 → 33792 bytes.

### why the pin must be decided per batch (run 4)

the run-4 dump (c4feec7, pin 0.62) shows what per-cluster pinning does to the
choose-adventurer screen. the camp left column comes through as batches like
`n 124, x -0.510..-0.233, y 0.055..0.870` with six clusters — eg
`0.237/0.323`, `0.829/0.081`, `0.257/0.388`, `0.739/0.050`, `0.261/0.277`,
`0.416/0.569` (mean/span). the 0.829 and 0.739 clusters pass the 0.62 line, so
they pinned to `+1` and jumped up ~20-40 px relative to their siblings in the
same draw: one piece of art, torn in two. every cluster of every one of those
batches sits below the same 0.85 line except scattered singles at 0.85..0.94 —
and the **hud itself** sits at authored mean 0.86-0.92, so no threshold can
separate art from hud. the separation is structural: pure-hud batches are one
compact cluster; artwork and mixed passes are not.

supporting measurements (`userimg/old_SicnI_0.png` = `a50fe75`, a direct
1.3148x crop of the 1280x960 device frame, no 2d scaling — i.e. authored
coordinates):

| element | px | ndc y | mean |
|---|---|---|---|
| banner paper body | 77..236 | 0.625..0.878 | 0.752 |
| banner text band | 138..185 | 0.706..0.781 | **0.743** |

the hud in the approved build (`userimg/il49Y_0.png`): hearts/health at
`x ≈ -0.96..-0.90`, bombs/ropes `-0.86..-0.70`, money `+0.40..0.47`, timer
`+0.64..0.75`, level `+0.82..0.86`, all `y ≈ 0.88..0.96` after pinning. the
left-hud and camp-art x ranges do not overlap (`-0.96..-0.70` vs `-0.51..-0.23`),
and the one x overlap that does exist — banner right scroll end `x 0.36..0.53`
vs money `x 0.40..0.47` — never materialises because the hud is not drawn on
the choose-adventurer screen.

### dead ends (both were tried, both are wrong)
- **the per-type scale table `0x22d79c3c`** (2d ui pass `0x22c5e2e0`, entries
  0/6 = 0.75). removed with `ui_aspect_fix` in `403f705`; the build that
  carried it crashed before any code path touched it (`uifix=0` in the log, and
  the write never ran). treat `0x22d79c3c` as unexplained, not disproven, but
  it is not needed — the viewport fix is upstream of it.
- **scaling the emitters.** the 8 emitters behind dispatch table `0x22d78aa0`
  (`0x22b91700` .. `0x22b94d30`) do compute `half_w = 1.7778 * half_h`, but
  that is the **culling rect for a text run**, not the glyph size. glyph sizes
  come from the run struct (`+0x28/+0x30` etc.) built by `0x22c4df60`. changing
  `1.7778` there only changes culling.

### also worth knowing
`aspect_globals = 1` in the shipped ini is **misleading**: the code
unconditionally logs `[skip] aspect_globals off (preserves full menu framing)`
even when the ini says 1. left as-is, but do not trust that line.

**the camp's gates and ladder are not in the 2d ui layer.** run 9 walked a whole
camp frame (the one in the user's screenshot: ladder in the centre, four gates,
choose-adventurer banner) and the only ndc batches it contains are the banner,
its two hanging ropes and the dim/fade fills. no gate, no door panel, no ladder,
no `A` prompt anywhere in the table. so their placement cannot be moved by
anything in this file's `ui_2d_*` block — three pins of ui-layer work (pin5/6/7)
have not touched them, and that is expected rather than mysterious. whatever is
wrong there lives on the world/level side: the camp is drawn through the camera
(`m11 = m00 * 4/3`), or in a pixel-space pass this mod leaves alone by design
(`UI_2D_NDC_Y` 8.0 guard). locate the draw **before** writing another rule; the
shape table now holds 1200 ndc shapes so a fresh camp frame is no longer truncated
(hit the 400 cap in run 9, which is why earlier logs looked "clean").

---

## 9. address reference

all rvas are relative to the module base (`[*] base=0x...` in the mod log).

### display / canvas state
| rva | meaning |
|---|---|
| `0x22e1e798` | lazy-init singleton slot → display object (**beyond the window `fx.py` maps; runtime only**) |
| `obj + 0x10` | → display struct |
| `+0x8` / `+0xc` | canvas `w` / `h` |
| `+0x81034` / `+0x81038` | pre-scale mode `w` / `h` |
| `+0x8103c` | scale percent (40..100) |
| `+0x804e9` | aspect mode byte (`2` ⇒ full rect, else centered 16:9) |

### the two override groups hiding behind `no_letterbox`
| rva | patch | in which function |
|---|---|---|
| `0x22cc1966` | `76 0e → eb 11` viewport w | `0x22cb8e30` (computes the centered 16:9 viewport rect, offsets `(h−vh)/2`, `(w−vw)/2`) |
| `0x22cc197f` | `76 0c → eb 0c` viewport h | " |
| `0x22ce40bc` | `3c 02 74 53 → eb 55 90 90` canvas | `0x22ce3f10` (clamps canvas to 16:9 unless mode==2) |
| `0x22ce41fa` | `76 0d → eb 33` scissor | `0x22ce4130` |

these are split behind `vp_full` / `ui_canvas_full` so each can be tested alone.
**both default to 1 = the shipped behaviour.**

### projection
| rva | meaning |
|---|---|
| `0x22d12410` | `m00` — **rewritten by the game every frame**; `watchdog_thread` re-applies it |
| `0x22d12424` | `m11` (stock `2.41421`) |
| `0x22d0c4a0` | `1.7777778` (aspect main, 58 refs) |
| `0x22d0d620` | `0.5625` (aspect inv, 1 ref) |

### ui construction
- `0x22c6123a` builds a UI element with a runtime `× 1.7778`
  (`xmm15 = 0.04 × xmm7`, `xmm9 = xmm15 × 1.7778`).
- `0x22c5d870` "Drawing HUD elements" = the **whole** 2d ui pass. builds the
  batch via `0x22c5f050` (layout, calls `0x22b914b0` the element dispatcher)
  and `0x22c5e2e0` (per-type pass), then flushes once:
  - `0x22c5e12d` `call 0x22cc5d80` batch reset
  - `0x22c5e190` `jmp [rax+0x1b8]` draw (the only such jmp in `.text`)
  - `rax` comes from `[0x22e18c68]`, the cached `[[0x22e1e798]+0x10]` display.
- **`0x22c5e132` is the `ui_letterbox` hook site** (17 bytes, see §8).
- element dispatcher `0x22b914b0` -> dispatch table `0x22d78aa0` -> 8 emitters
  `0x22b91700 0x22b91cf0 0x22b92400 0x22b92ad0 0x22b932a0 0x22b93ab0
  0x22b94440 0x22b94d30`. vertex = 24B `{x, y, z=0, u, v, color}`; the
  `1.7778` they multiply is the **text-run culling rect**, not glyph size.
- `0x22c70080` is *not* the ui flush — it is the post/composite chain (half
  width viewports, ends in a full-screen textured quad, material 0x29).

### viewports in the frame (`0x22cb8e30`)
only three `[reg+0x48]` calls in the whole function:
- `0x22cbfee9` -> `(0, 0, disp+8, disp+0xc)` = **2d canvas**, active from
  `0x22cbfeeb` through `0x22cc192b` (world *and* hud *and* menus).
- `0x22cc192b` and `0x22cc19d1` -> the centred 16:9 / full-window pair
  (`[disp+0x804e9] == 2`), which is what `no_letterbox` patched.
scissors: `0x22cbffd0`, `0x22cc00ef`, `0x22cc18df` (`[reg+0x78]`).

### the game rewrites `.rdata` at runtime
`m00` at `0x22d12410` is overwritten each frame and re-applied by the watchdog.
so runtime `.rdata` writes are precedented — but FEX translation caching still
makes runtime `.text` patching unreliable.

---

## 10. static-analysis tooling

`tools/fx.py` opens `Spel2.exe` by relative path, so run
it from the repo root. it needs `capstone` and `numpy`.

### section map (this is the thing that wastes your time)
```
.text  = rva - 0xc00    raw 0x400      size 0x22d08200
.rdata = rva - 0x1a00   raw 0x22d08600 size 0xe0400
.data  = rva - 0x2600   raw 0x22de8a00 size 0x3600     <-- only 0x3600 bytes
.pdata = rva - 0x3a00   raw 0x22dec000 size 0x1cc00    (function begin/end)
```

### gotchas that have cost time
- **`.data` is only mapped for its first `0x3600` bytes.** reads past that fail.
  `0x22e1e798` (the display singleton) lives well beyond it → **runtime only**,
  which is why section 7 exists.
- **capstone silently yields nothing if the start offset lands on an invalid
  byte.** pass `skipdata=True` or start at a valid instruction boundary.
- **`Spel2.exe` is not in this repo** — copy a stock one into the root (gitignored).

---

## 11. how to iterate

1. change `mod_43aspect.c` / `.ini`
2. `make release`
3. push the dll (and ini) over ssh
4. **the user launches or relaunches the game** (section 12)
5. read `mod_43aspect.log` + `spelunky.log`, screenshot via adb, and read live
   memory with `tools/snap.py`

**one issue at a time. one reversible change per test. keep the known-good
baseline intact.** distinguish what you verified from what you predict.

---

## 12. launching the game — read this before trying

### what works from ssh: start without steam (2026-10-20)

```sh
pid=$(ssh droiddeck 'pgrep -n konsole || pgrep -n lxqt-panel')   # any desktop process
ssh droiddeck "python3 /root/ddlaunch.py $pid /usr/local/bin/droiddeck-gpu \
    /usr/local/bin/droiddeck-proton-run --app 418530"
```

`droiddeck-proton-run --app` is droiddeck's "Start without Steam": it runs the
game's proton directly, with no linux steam client, so the client-update trap
below never fires. `droiddeck-gpu` puts it in a gamescope on the desktop's
wayland, and `ddlaunch.py` borrows a desktop process's env so the ssh shell can
reach that wayland. Spelunky boots to `Game initialized` in ~6 s and plays
(screenshots via adb work). to stop it, SIGTERM the processes the run started
(Spel2.exe, its steam.exe / proton / wine helpers / wineserver, the gamescope +
gamescopereaper + `Xwayland :1` from droiddeck-gpu); gamescopereaper and one
winedevice may need SIGKILL. never touch `Xwayland :0`, kwin, lxqt or
droiddeck-session. still: the user normally launches; this is for test runs.

### what breaks steam: driving the client

the supported path is droiddeck's own: the desktop menu, or the android app's
gaming session. a desktop menu click runs
`steam steam://rungameid/<appid>`, which goes through
`/usr/local/bin/droiddeck-steam-launch` → `droiddeck-session steam` →
gamescope. to ask the app for a session from inside, write
`ui=...\nurl=...` into `$BL_LAUNCH_DIR/steam-launch`.

**driving it from ssh does not end well.** it *appears* to work: copy the env of
a desktop process (e.g. konsole) out of `/proc/<pid>/environ` with
`tools/ddlaunch.py`, run `/usr/local/bin/steam steam://rungameid/418530`, and
steam + proton + the game all come up — the game even reaches
`Game initialized` in `spelunky.log`. **then it exits.**

the reason is not the mod (the validated head build does exactly the same
thing). valve retired the `steamdeck_publicbeta` channel, so the client flips

```
Client beta changed from 'steamdeck_publicbeta' to ''
```

and then fails the update it triggers — 28 lines of

```
Package file bins_linuxarm64.zip.vz... missing or incorrect size
```

leaving a client that cannot launch anything (celeste fails identically). it
also leaves a "this beta has ended, restart steam" dialog on the user's screen.

**so: have the user launch the game.** if you do launch anyway, clean up after
yourself by killing the top level `droiddeck-session steam` pid and the steam
client, then let droiddeck start steam itself — and expect the user to need a
session restart before their games work again.

---

## 13. conventions

- **evidence only.** never invent an address, offset, or byte pattern. if it was
  not read out of a binary, a log, or memory, say so.
- **one issue at a time**, in isolation. do not bundle unrelated fixes.
- **test one reversible change at a time** and preserve the known-good baseline.
- **verified ≠ predicted.** label them.
- comments in this repo are lowercase, terse, and explain *why*, never *what*.
- the ini is hand-tuned — do not push it as a side effect of a build.

---

## 14. history (so it is not re-litigated)

- `a50fe75` "CHOOSE ADVENTURER SCREEN WORKS. INGAME 4:3 WORKS." — skinny hud and
  skinny pause menu already listed as the remaining breakage.
  `git log -S 'unscale'` shows "C) Menu aspect unscale removed" was already gone
  at the initial commit, so there is no recoverable prior implementation.
- `2339b70` fix crash after a few seconds in game.
- `307c9bc` fix violent camera jitter on level entry / rope descend.
  root cause: the camera's visible height is `zoom × 0.2667 × 2` tiles, so
  `custom_zoom >= 15.0` is a full 8-tile room and leaves the vertical clamp with
  no valid range. **13.5 is vanilla. do not raise it.**
- `403f705` added `probe_2d_canvas` (2d ui scale table probe) + `ui_aspect_fix`.
  **this build crashed on device and its dll is unusable.** what is actually
  established about that crash:
  - the log shows `SUCCESS`, `uifix=0`, `[skip] ui_aspect_fix off`, and **zero
    `[matrix]` / `[2d]` lines**. it is not a truncated log: `apply_patches`
    truncates on every load, so that is the whole of the crashed run.
  - `probe_2d_canvas` therefore **never printed** — the probe is not the cause
    and its output does not exist to read. `matrix_report_thread` sleeps 2500ms
    before its first line, and `scan_and_patch_live_matrices` runs *before* that
    line, so a death in the first poll window produces exactly this log shape.
  - `ui_aspect_fix` was off, so the `0x22d79c3c` write never executed either.
  - the steam session log has `mmap() failed: Cannot allocate memory`, and
    `spelunky.log` stops at "Renderer initialized" — i.e. death during d3d
    bring-up, ~2.5s after dll load.
  leading explanation: not the new code (nothing new ran) but the unchanged
  `scan_and_patch_live_matrices` heap scanner racing d3d init, with the larger
  binary/ini shifting heap timing. **not proven.** if the next build dies the
  same way, gate the scanner behind a flag before concluding anything.
- `95818d0` `ui_letterbox` — hooked `0x22c5e132`, which is **after** the hud draw
  at `0x22c5e12d`. device run: menus/loading became a 16:9 letterbox (the band
  leaked out of the hud pass, nothing restored it) and the in-level hud icons
  were untouched (they were drawn before the band went up). one wrong
  instruction boundary, both symptoms. **do not re-try this site.**
- **`2d-ui-scale` (`0c5ff71`)** — `307c9bc` + one mechanism: `ui_2d_scale`
  (default 1) scales y of the pending 2d batch by 720/960 at the one draw call
  `0x22cc5d80`, leaving `|y| ≈ 1` full-screen quads alone. `ui_letterbox` moved
  to `0x22c5e12d` (wraps the real draw and restores the canvas viewport) and
  defaulted off; the two are mutually exclusive. 27136 -> 30720 bytes, log line
  `[2d] first flush: canvas ... stride ... verts ... idx ... sy ...` proves the
  hook ran.
- **device run of `0c5ff71` (verified, and the reason for the next commit)** —
  proportions are fixed everywhere ("the ui actually FINALLY looks square"),
  pause menu fixed, and the level itself is untouched. two things stayed wrong,
  both **position**, not size:
  - in-level hud bar sits 120 px under the top of the window instead of ~22 px,
    and the CHOOSE ADVENTURER banner / Quick Select do the same. that is the
    centre-anchored scale: `0.75 × y` moves anything authored near an edge
    toward the middle by 25 % of its offset.
  - on level entry the stretched (unscaled) hud is visible for ~1 s and then
    squares out — the canvas is still 1280x720 during the loading screen, so
    `sy` is 1.0 until the level pass starts. not a bug in the hook.
  batch census from the same log: n = 4 (most common), 6, 8, 12, 16, 18, 24, 28,
  36, 48, 102, 248, 304, 584 vertices, and every 248/304/584 batch scales **0**
  vertices — they are not ndc geometry (the `|y| >= 0.995` guard skips them),
  which is why the world never got squashed.
- **`ui-2d-pin` (`57cf2ab`)** — keep the 0.75 y scale but anchor it per
  element: quads grouped 4 at a time, clustered by y centre within
  `ui_2d_cluster`, and a cluster that is compact (`<= ui_2d_pinspan`) and past
  `ui_2d_pin` from the centre scales about the screen edge it hugs. also adds
  `ui_2d_dump`, which logs each distinct batch shape once. 30720 -> 33792 bytes.
  verified as maths on the host (extracted the real functions and ran them on
  synthetic batches): hud bar 0.86..0.94 -> 0.895..0.955 (pinned), pause panel
  -0.55..0.55 -> -0.413..0.413 (not pinned, unchanged), bottom bar pinned to
  -0.955..-0.895, mid-screen text unchanged, full-screen dim left at ±1.
- **device run 2 of `57cf2ab`** — hud icons jump to the top correctly and the
  level/pause menu are untouched, but **every text batch stayed behind**:
  "the hud elements went to the top (nice) but the text didnt come with it".
  cause: text batches are `n = 6 / 18 / 30` (quads emitted as two triangles),
  and the `n % 4 == 0` guard sent them down the centred-scale fallback with no
  pinning at all. the choose-adventurer banner looks like *two* banners for the
  same reason (one copy of the element pinned, its sibling left in the band),
  and the main menu's description bar is split from its text.
  the dump also revealed the shape table was being eaten by world geometry
  (`y ≈ 100`, `z = 5..40` riding the same flush) before any ui batch was seen.
  **ssh into the container was down during this run** (`kex_exchange_identification:
  connection closed`) — the adb-side forward was fine; the device-side listener
  was gone. read the log and push builds through `/storage/emulated/0/Download`
  instead (`adb exec-out cat .../mod_43aspect.log`, `adb push ... /sdcard/Download/`),
  which works because the shim mirrors the log there and copies from there on
  launch.
- **`ui-2d-pin2` (`c4feec7`)** — the vertex walk no longer requires a multiple
  of 4 (the tail group takes what is left), so text batches pin with their icons.
  `ui_2d_dump` now skips non-ndc batches, keeps 160 entries, buckets y to 1/8 so
  a sliding element logs once, and prints the per-cluster decision (mean, span,
  anchor) plus the flush index so a doubled element is visible in the log.
  33792 -> 34304 bytes. verified on the host for n = 4 / 6 / 18 / 30 and the
  mixed case.
- **device run 4 of `c4feec7`** — **the hud is fixed** ("the hud has finally
  been fixed"): icons and text both go to the top edge, level and pause menu
  stay put. the remaining complaint is the choose-adventurer screen: "the
  choose adventurer quads and text is still fucked". the dump explains it: the
  camp is drawn as stacks of multi-cluster 2d batches (x -0.51..-0.23,
  y 0.06..0.94, up to n 1076) whose clusters straddle any threshold — the
  0.62..0.94 ones pinned in place while their siblings scaled about other
  anchors, tearing each stack (the checkpointed `a50fe75` measurement: "Ana"
  +0.667 -> +0.750, slot row 2 +0.238 -> +0.179, lower slots -0.371 -> -0.279 —
  positions that must match the camp behind them, not move). also confirmed:
  text batches arrive as n = 6/18/30 (2-tri quads), so the n%4 guard removal was
  the right fix for the hud; the dump table (160 entries, dedup on
  n/nidx/y-buckets only) filled before the visible hud ever logged — read the
  left-hud x ranges off `userimg/il49Y_0.png` instead of the log.
- **`ui-2d-pin3` (`ee60541`)** — pinning is now a **per-batch** decision:
  a batch is pinned only when **every** cluster in it is compact
  (`<= ui_2d_pinspan`) and past `ui_2d_pin`; otherwise every cluster scales
  about its own mean, so no authored position moves at all (heights still get
  0.75x). `ui_2d_pin` default and ini raised 0.62 -> 0.85 (hud authored means
  0.86..0.92; banner text 0.743). `ui_2d_pinned_runs`/`ui_2d_anchor` replaced by
  `ui_2d_cluster_edge`/`ui_2d_batch_pins`; the dump's anchor column now shows
  the final decision. verified on the host against the extracted real functions:
  hud n=4/6/18/30 all pin (0.86..0.94 -> 0.895..0.955 as approved); the run-4
  camp shape (clusters 0.829/0.739/0.417/0.261/0.237) pins **nothing** and every
  cluster centre is preserved; the banner-text-like flat run at mean 0.885
  still pins, which is why the threshold must stay <= the hud means. the host
  harness now generates `alg.inc` straight from `mod_43aspect.c` so it cannot
  drift again. 34304 -> 34304 bytes.
- **device run 5 of `ee60541`** — validated: "wow youve done it. only a few
  weird glitches left". hud stays pinned, choose-adventurer alignment good.
  the three leftovers (camp rope break, pause/dialog text low, 16:9 background
  flash on zoom transitions) are diagnosed from this run's dump in the
  "run 5 refinements" block of §8 — the mechanics live there, not here.
- **`ui-2d-pin4` (this commit)** — `ee60541` + the three run-5 fixes, each one
  an independent rule with its own ini key so any of them can be backed out
  alone:
  - `ui_2d_pinx` (0.30): a cluster only pins when its x centre is past this —
    the rope's top segments sat dead centre and pinned, jumping while the
    segments below scaled, so the rope broke. all run-5 pinned hud clusters
    (`x 0.45..1.01`, `0.78..0.93`) keep pinning.
  - `ui_2d_rowgap` (0.05): a quad whose y box is further than this from the
    previous quad's box starts a new element, so the pause text block (6 rows
    on a 0.135 pitch, previously merged into one cluster by the 0.20 centre
    rule) keeps its authored row positions and only its heights shrink.
    the rule can only split, never merge, so tiling art is preserved; set 0 to
    restore `ee60541` behaviour exactly.
  - fill test in `ui_2d_apply_runs`: the old per-vertex `|y| >= ui_2d_edge`
    skip sheared art overshooting the frame (a camp column with `y 0.147..1.297`
    came out half-scaled); now a **cluster** spanning `-0.99..+0.99` is left
    alone (fade/dim/blur fills) and everything else scales all its vertices.
    **this is the change that banded the level** — see run 6 below; the fix is
    `ui-2d-pin5`'s cover test plus the explicit world guard.
  toggle line now prints `px%s rg%s`; toggle buffer 384 -> 448.
  34304 -> 35840 bytes, md5 `1fbba279d11789e62876043eae423d8a` (+ ini
  `84a8cb67a51eb3f2000135727fe2584e`), pushed to `/sdcard/Download/`
  (md5 verified on device). host harness re-run: 19 cases pass, `alg.inc`
  regenerated from source.
- **device run 6 of `ui-2d-pin4` — rejected, reverted within the minute.**
  "put bands everywhere… nothing got fixed it only made it worse": black bands
  across the level (in-level and behind the pause menu), while the pure-2d
  screens looked normal. cause: the fill rewrite dropped the per-vertex
  `|y| >= ui_2d_edge` line, which was also the *only* thing keeping the
  world's 2d geometry (y in the tens, see §8) out of the scaler — so every
  world cluster squashed 0.75 about its own mean and the level banded. the
  dump hid it by filtering non-ndc batches. recovery: `ee60541` dll + ini
  back to `/sdcard/Download/` (md5 `b2a5b65b…` / `73a2ec4f…`, also kept in
  `rollback/ee60541/`), so a relaunch runs the validated build. run 6's log is
  `run6.log` next to `run5.log`.
- **`ui-2d-pin5` (this commit)** — the same three fixes with the world guarded
  for real: `UI_2D_NDC_Y` 8.0 (mirrors the dump's own filter) skips any vertex
  in the tens, so the world can never scale whatever the element rules do; the
  fill test became a cover test (`UI_2D_FILL_SPAN` 1.8 tall touching an edge),
  which also stops the run-6 shape 14 quad (`y -1..0.896`) from lifting its
  bottom edge; `ui_2d_fill` (default 1, 0 = ee60541's per-vertex line) is the
  escape hatch and is in the ini. verified on the host: a world batch scales 0
  verts in both fill modes, the cover quads scale 0, the overshoot column
  still scales 4 of 4 (2 of 4 with fill 0). 35840 -> 36864 bytes, md5
  `867ee323ef72fb81e066395fb3e2382f` (+ ini `9708cb679dc02a67c92c5d7db42dc8f1`),
  pushed to `/sdcard/Download/` (md5 verified on device).
  **not yet on device.**
- **device run 7 of `ui-2d-pin5`** — bands fixed, hud good, camp art good, rope no
  longer breaks; three reads left. (1) pause/confirm text rows **uneven**: shape
  80 split into title / one item / items 2-5 chained (`-0.150/0.486`), giving
  0.135, 0.192, 0.101 pitch instead of the authored 0.135. the row rule compared
  the next quad against the *previous quad*, so one survived transition kept the
  chain alive. (2) rope showed **bands** and the camp gate pieces sat wrong: the
  same rule was cutting tiling strips (rope segments are 0.016 wide, 0.03 tall,
  0.06 pitch — every cut opened a 0.25x gap). (3) the main menu's dark backdrop
  showed hard top/bottom edges ("black bars") — not yet diagnosed; the 16:9 -> 4:3
  head-snap on the camp's first second is the canvas sitting at 1280x720 (sy 1.0)
  until the level pass starts, also undiagnosed.
- **`ui-2d-pin6` (this commit, `a425bb7`)** — the row split is now text-only and
  order-independent: a candidate is compared against the **run's whole box**, and
  may only break when it is glyph-sized (`UI_2D_ROW_H` 0.12, `UI_2D_ROW_W` 0.06),
  the run is already `UI_2D_ROW_RUN` 0.06 wide and `UI_2D_ROW_N` 6 quads long.
  letters pass; art pieces (>0.06 wide), the rope (<0.06 wide) and short stacks
  never do. dump: cap 160 -> 400 (the rope never made the table), `[2d] canvas`
  line per canvas change, and quad boxes + first-vertex alpha for rope-like
  columns and backdrops. host: 6-row letter block keeps all six authored row
  centres, rope strip stays one element, all 19 old cases unchanged.
  36864 -> 37888 bytes, md5 `95f114c998bddd83cb8c1f2e946eab57` (+ ini
  `eebc77ed2d8e0e187f3b76ac05cce409`), pushed to `/sdcard/Download/`
  (md5 verified on device). `rollback/ee60541/` still the known-good fallback.
- **device run 8 of `ui-2d-pin6`** — "no difference, same exact issues". the log
  (`run8.log`) explains it: the pause text is `n 312 idx 312`, a triangle list,
  so it is 52 six-vertex letters and the 4-vertex grouping straddled them — the
  row rule could never see a row gap, and shape 78 still shows the same three
  clusters. the rope at `x -0.847` is a column of one-quad batches (`n 4 idx 6`,
  0.033 tall, 0.049 pitch) with other draws between them, so per-segment scaling
  opens the authored gaps and the pinned top segments jump. also in the log: no
  canvas change at all this run (the camp never went through 1280x720), and the
  gate pieces never made the (deduped, filtered) shape table.
- **`ui-2d-pin7` (this commit)** — two fixes. (1) the group size comes from the
  batch: `n * 6 / idx` when `idx % 6 == 0`, clamped 3..6, else 4 — 4 for indexed
  quads, 6 for the triangle-list letters, so the row split finally lands on row
  boundaries. (2) narrow single-quad batches chain by column (4 slots, x match
  ±0.005, y within -0.005..+0.30 of the quad above): a rope's segments then share
  one anchor and the column scales as a piece instead of opening bands. host: the
  same 6-row block merges into 3 clusters with vpq 4 and keeps all 6 row centres
  with vpq 6; 8 single-batch rope segments take the top anchor in turn (gaps
  0.030 -> 0.0225) and a wide art quad between them does not break the chain; all
  old cases unchanged. 37888 -> 39424 bytes, md5
  `c11b3f273d3119fc4de4ca5238a22487` (ini unchanged, `eebc77ed…`), pushed to
  `/sdcard/Download/` (md5 verified on device). **not yet on device.**
- **device run 9 of `ui-2d-pin7`** — "nope still seeing this" + a camp screenshot
  (the ladder in the centre, the four gates, the banner). the log (`run9.log`)
  says the vpq fix **worked**: shape 108 (`n 312 idx 312`) now reports `v 6` and
  five clusters instead of three, with rows 2-5 on their authored 0.14 pitch —
  only the title is still merged (four letters vs `UI_2D_ROW_N` 6). the rope did
  **not** get fixed: the camp's banner ropes arrive as `n 8` batches holding a
  segment of **each** rope (means 0.86 apart, so `ui_2d_cluster` splits them),
  which pin7's `ng == 1` batch test rejects outright, and four slots would have
  been too few for the camp's columns anyway. the dump's own anchor column was
  printing `pin ? ±1 : mean` and so could not tell a working chain from a broken
  one — that is why run 9 needed a fresh script instead of a grep. also in the
  log: canvas stays 1280x960/sy 0.75 for the whole run (no 720 phase), and the
  camp's gates/ladder produce **no** ndc batches at all — they are not in the 2d
  ui layer, which is why three pins of ui-layer work have not moved them.
- **`ui-2d-pin8` (this commit)** — the chain is per **cluster** (12 slots, so a
  column survives the camp's other columns) and the row split's `UI_2D_ROW_N`
  drops 6 -> 4 so a four-letter title is a line. dump: the `g` line prints the
  anchor the pass will really use (chain lookup included), and the shape table
  cap goes 400 -> 1200 — the camp's ndc pieces were falling off the end of the
  table, which is the other half of why gates have never appeared in a log. host:
  the two-rope flush keeps each column on its own anchor (order-independent,
  checked with the columns swapped), a four-letter title + its first item now
  report two lines (`0.354 0.214`) instead of one, and all 21 old cases unchanged.
  39424 -> 39936 bytes, md5 `5d1c6c14e637a636cbaa1bf043ffa131` (ini unchanged,
  `eebc77ed…`), pushed to `/sdcard/Download/` (md5 verified on device).
  **not yet on device.**
- **`ui-2d-pin9`** (uncommitted) - rope fix (finding A above: the column anchor
  beats the batch pin), `ui_2d_bars` gated rule (default 0, finding B), dump gets
  a `bb` line and `q` boxes for multi-level batches, toggle line shows `bars%d`.
  the stray `canvas_mode` diff (never ran on device, no `cvm=` in the pin8 log) is
  parked in `rollback/canvas_mode_wip.patch`. host harness recreated
  (`tools/gen_alg.sh` -> `tools/alg.inc`, `tools/test_alg.c`, T1-T8 pass).
  39936 -> 40960 bytes, md5 `c7a18189fda3e2f2c9114f673a0fb16f`, ini
  `58331b93b3b52ab0adfefa4199b3db4b` (adds `ui_2d_bars = 0`); copies in
  `rollback/pin9/`; pushed to `/root/Storage/Download/` (md5 verified on device).
  **not yet on device.**
- **`ui-2d-pin10`** (uncommitted) - strip continuation in `ui_2d_cluster` (rope
  shape 272), `ui_2d_center` toggle (ini 1, code 0), frame trace (`[2d] tr`),
  toggle line shows `ctr%d`; see the run 10 result in section 8. host T1-T11 pass.
  40960 -> 43008 bytes, md5 `3b378ea568f219660677357a60cfe557`, ini
  `be370e528092091ec87a25f034cf5f4b`; copies in `rollback/pin10/`; pushed to
  `/root/Storage/Download/` (md5 verified on device). **not yet on device.**
- **device run 11 of `ui-2d-pin10` — crashed ~5 s into startup, and not in the ui
  code.** `steam-418530.log` (in `/root`, the proton log) has the record: read
  access violation at `mod_43aspect+0x2c10` on `0x73fc978000` (a page boundary),
  thread started via `kernel32` (a mod thread, not the render hook). `+0x2c10` is
  the `movss (%rbx)` of `scan_and_patch_live_matrices` inlined into
  `matrix_report_thread`: it dereferenced every committed rw region raw, so a
  region freed between `VirtualQuery` and the read faulted. this is the
  403f705 crash's leading explanation, now proven; the ui_2d changes only shifted
  the timing. the scan has **never** produced a `[live]` line in any kept log.
  the mod log stops at `batch 2000`, which is what that crash looks like.
- **`ui-2d-pin10b`** — pin10 + the scan fixed and gated: reads go through
  `ReadProcessMemory` in 64 KB chunks (60 B overlap), patches and the watchdog's
  re-lock through `WriteProcessMemory`, so a vanished region fails instead of
  faulting; and new ini key `live_scan` (default 0) gates it, logged as
  `[skip] live_scan off`. ui_2d code identical to pin10. host T1-T11 pass,
  0 format warnings. 43520 bytes, md5 `351c193a6f1608e835e99b7465f7077a`, ini
  `ac843ff976e7acb6e0f3fcc664ae482c`; `rollback/pin10b/`; pushed and md5
  verified on device. **not yet on device.**
  when a build dies at startup: `grep -n 'page fault\|backtrace' /root/steam-418530.log`
  gives the module offset; map it with an unstripped build (same flags minus `-s`)
  and `objdump -d`.
- **device run 12 of `ui-2d-pin10b` (committed `002431b`)** — "the rope banding
  and the text centering glitch is completely fixed". left: backgrounds draw
  16:9 during screen transitions and pop to 4:3 when they end (main menu black
  bars, camp zoom-in, zooming into options), and the camp fog jumps every few
  seconds. canvas stayed 1280x960 the whole run, so not the old 720 phase. the
  frame trace (`[2d] tr`) explained all three:
  - main menu: the tunnel image is `x +-0.533, y +-0.948` behind the `+-1` cave
    fill. 1.896 tall but not touching an edge, so the fill test scaled it to
    +-0.711; the screenshot's black bar edge sits at 0.715.
  - zooms: the scene is composited through quads with the frame's own ndc
    proportions (`+-0.620 -> +-0.775 -> +-1.07 -> +-2.9`, width == height).
    scaled while under the fill size, unscaled once they hit +-1: the pop.
    the centre rope zooms with it (`+-0.662 -> +-1.001`), same pop.
  - fog: one cluster wider than the screen (`x -1.37..1.39`), x centre drifting
    0.013 <-> 0.021 across `UI_2D_CENTER_X` (0.02), so `ui_2d_center` flipped
    its anchor 0 <-> -1.001: ~120 px jumps.
- **`ui-2d-pin11`** — `ui_2d_keep()` decides which clusters stay at full height:
  span >= 1.8 (edge requirement dropped), frame-shaped (`|w - h| <= 2% h`,
  h >= 0.5), or a strip of >= 4 tiles <= 0.15 wide and >= 0.5 tall (the rope).
  `ui_2d_center` only applies inside the frame (`xmin >= -1 && xmax <= 1`).
  host T12 covers each rule plus what must still scale (pause board, the
  +-0.85 arch, banner frame, +-0.971x+-0.88, 3-tile column, 0.4 square), and
  the fog with its logged centres; mutation runs against the old rules fail
  T9/T12. T1-T7/T11 record hashes unchanged from pin10b. 43520 bytes, md5
  `02f1cede2666a4d95bf3774e799b614b`, ini unchanged (`ac843ff9...`);
  `rollback/pin11/`; pushed, md5 verified on device. committed as `71fe81c`; user-verified
  on device: "working perfectly besides the lag spike on pause" (the spike is the
  first-pause log dump, fixed in the single-dll release below).
- **release: single dll (`09ce41e`..`93b0bc7`)** — the shim and the mod dll
  collapse into one freestanding `discord_game_sdk.dll` built from
  `mod_43aspect.c`. six changes:
  - `09ce41e` cache log paths, drop the Download mirror, quiet defaults
    (`debug_log` off). this fixed the first-pause lag spike: `log_raw` opened and
    closed the log plus a Download mirror on every line, and the first pause
    fired the `[2d] tr` ring dump (97 lines) plus ~52 new-shape lines, ~174
    lines in 1-2 frames on the render thread (run12.log 645-818). later pauses
    dedup, so only the first one hitched.
  - `8d21193` code defaults match the tuned ini.
  - `de740d5` heap hint range patch for android's 39-bit va, two bytes in
    `Spel2.exe`: `movabs rbx` `0x7017F800000` -> `0x17F800000` at RVA
    `0x22CBD624`+7, and `and ecx,0xFE` -> `0x7E` at RVA `0x22CBD6BA`+2. bytes
    are verified before writing; an exe already patched by the browser tool is
    tolerated.
  - `2a58398` audio workaround ported from the shim (`force_audio`: re-apply
    local.cfg fmod bus volumes for the first minute; there was no sound on
    android without it).
  - `9be37e6` exports a `DiscordCreate` stub that returns failure, so the dll can
    stand in for the real sdk (rich presence disabled).
  - `e211626` / `3c7fba4` single-dll Makefile, shim + probe moved to `legacy/`,
    host test pinned to its original ui_2d defaults. `93b0bc7` rebuilds the dll
    from the final source.
  rollback: `rollback/pin11-shim/` holds the shim dll + the pin11 mod dll/ini.
  restore by putting the shim dll back in the game dir as `discord_game_sdk.dll`
  and the mod dll/ini in `/root/Storage/Download/`. **not yet on device.**
