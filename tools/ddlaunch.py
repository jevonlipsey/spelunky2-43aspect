#!/usr/bin/env python3
# start a command with the environment of a process that already has the
# desktop session (wayland, pulse, BL_*), so an ssh shell can reach it.
#
#   ssh droiddeck "python3 /root/ddlaunch.py <pid> <cmd...>"
#
# moot for launching games: see the warning in .agents/system.md. it is kept
# because it is the right tool for anything else on the desktop (a screenshot
# tool, a shell inside kwin, a probe binary).
import subprocess
import sys

src_pid = int(sys.argv[1])
argv = sys.argv[2:]

env = {}
with open(f'/proc/{src_pid}/environ', 'rb') as fh:
    for kv in fh.read().split(b'\0'):
        if b'=' in kv:
            k, v = kv.split(b'=', 1)
            env[k.decode()] = v.decode()

# mark us as a top-level entry into the session, like the desktop menu does
env.pop('BL_INSIDE', None)

log = open('/tmp/ddlaunch.log', 'ab', buffering=0)
log.write(('=== launch %r\n' % (argv,)).encode())
proc = subprocess.Popen(argv, env=env, stdout=log, stderr=subprocess.STDOUT,
                        start_new_session=True, cwd='/root')
print('launched', proc.pid, 'env_from', src_pid)
