CC     = x86_64-w64-mingw32-gcc
CFLAGS_ASPECT = -shared -O2 -Wall -nostdlib -fno-builtin -mno-stack-arg-probe -Wl,-e,DllMain
LDLIBS_ASPECT = -lkernel32

# -s strips the symbol table. drop it while iterating so anything that does
# crash still comes back with a usable backtrace.
CFLAGS_RELEASE = -s

DLL  = discord_game_sdk.dll
GAME = /root/.local/share/Steam/steamapps/common/Spelunky 2
SSH  = ssh -o BatchMode=yes droiddeck

.PHONY: all clean debug release deploy push_ini pull_ini logs test

all: release

release:
	$(CC) $(CFLAGS_ASPECT) $(CFLAGS_RELEASE) mod_43aspect.c -o $(DLL) $(LDLIBS_ASPECT)

debug:
	$(CC) $(CFLAGS_ASPECT) mod_43aspect.c -o $(DLL) $(LDLIBS_ASPECT)

clean:
	rm -f $(DLL) tools/alg.inc tools/test_alg

# the dll is the whole mod now (heap fix + 4:3 + audio), loaded by the game as
# its discord sdk. scp is broken on the device (no sftp subsystem), so pipe
# through ssh. always deploy from a release build: the unstripped one carries
# ~102KB of DWARF and this loader rejects it.
deploy: release
	ls -l $(DLL)
	$(SSH) 'cat > "$(GAME)/$(DLL)"' < $(DLL)

# the ini is deliberately NOT part of deploy. it holds tuning values you edit
# by hand to A/B aspect ratios, and pushing it every build silently reverts
# them. run `make push_ini` when you actually want to seed a fresh device.
push_ini:
	$(SSH) 'cat > "$(GAME)/mod_43aspect.ini"' < mod_43aspect.ini

# pull the ini back off the device so hand edits become the new baseline
pull_ini:
	$(SSH) 'cat "$(GAME)/mod_43aspect.ini"' > mod_43aspect.ini

logs:
	$(SSH) 'cat "$(GAME)/mod_43aspect_boot.log"' > mod_43aspect_boot.log || true
	$(SSH) 'cat "$(GAME)/mod_43aspect.log"' > mod_43aspect.log || true

# host checks: the ui_2d algorithm against its recorded cases, and the browser
# patcher's core against a stock Spel2.exe dropped in the repo root (gitignored)
test:
	sh tools/gen_alg.sh
	cc -O1 -o tools/test_alg tools/test_alg.c -lm
	./tools/test_alg | tail -1
	node tools/test_patcher.mjs
