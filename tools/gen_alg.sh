#!/bin/sh
# extracts the ui_2d algorithm out of mod_43aspect.c into tools/alg.inc, so the
# host test runs the shipped code and cannot drift from it.
set -e
cd "$(dirname "$0")/.."
{
  awk '/^#define UI_2D_NDC_Y/{p=1} /written by the patch functions/{p=0} p' mod_43aspect.c
  awk '/^static int   g_do_ui_2d      = 1;/{p=1} /one line per distinct batch shape/{p=0} p' mod_43aspect.c
} > tools/alg.inc
test -s tools/alg.inc
grep -q ui_2d_apply_runs tools/alg.inc
grep -q UI_2D_BAR_D tools/alg.inc
