#!/usr/bin/env bash
# Verifies that every post-codegen patch is present in a generated tree (a new codegen silently loses them).
#   usage: tools/verify_pch.sh [generated dir]   (default app/generated/default); exit 1 if anything is missing.
# Called at the end of tools/codegen.sh; also usable alone after copying or regenerating a tree.
T=${1:-$(cd "$(dirname "$0")/.." && pwd)/app/generated/default}
H=$(cd "$(dirname "$0")" && pwd)
PY=${PYTHON:-python3}
fail=0
ko() { echo "MISSING: $*" >&2; fail=1; }
[[ -d $T ]] || { echo "no such tree: $T" >&2; exit 1; }
# each patch script's own --check (it locates the function's file by searching the whole tree)
"$PY" "$H/pch_no_volatile.py" --gen "$T" --check        || ko "pch_no_volatile (REX_GUEST_VOLATILE)"
"$PY" "$H/pch_no_global_lock.py" --gen "$T" --check     || ko "pch_no_global_lock (g_me_lockfree_atomics)"
"$PY" "$H/pch_ui_viewport.py" --gen "$T" --check        || ko "pch_ui_viewport (g_me_ui_width in sub_82238FE8)"
"$PY" "$H/pch_ui_world_to_screen.py" --gen "$T" --check || ko "pch_ui_world_to_screen (g_me_ui_y_scale in sub_827C07F0)"
"$PY" "$H/direct_calls.py" --gen "$T" --check           || ko "direct_calls (weak-alias direct calls __imp__sub_*)"
"$PY" "$H/pch_indirect_dispatch.py" --gen "$T" --check  || ko "pch_indirect_dispatch (REX_INDIRECT_DISPATCH switch)"
# independent greps (guard against a script that reports success wrongly)
grep -q 'g_me_lockfree_atomics' "$T/masseffect_pch.h"  || ko "g_me_lockfree_atomics not in masseffect_pch.h"
grep -q 'REX_GUEST_VOLATILE' "$T/masseffect_pch.h"         || ko "REX_GUEST_VOLATILE not in masseffect_pch.h"
grep -q '#if REX_INDIRECT_DISPATCH == 0' "$T/masseffect_pch.h" || ko "REX_INDIRECT_DISPATCH switch not in masseffect_pch.h"
cat "$T"/masseffect_recomp.*.cpp | grep -q 'g_me_ui_width'    || ko "g_me_ui_width not in any recomp cpp"
cat "$T"/masseffect_recomp.*.cpp | grep -q 'g_me_ui_y_scale' || ko "g_me_ui_y_scale not in any recomp cpp"
n=$(cat "$T"/masseffect_recomp.*.cpp | grep -c '__imp__sub_[0-9A-F]\{8\}(ctx, base);')
(( n > 1000 )) || ko "only $n direct __imp__sub_ calls (expected thousands)"
if (( fail == 0 )); then
  echo "verify_pch: all post-codegen patches present in $T"
else
  echo "verify_pch: FAILED; run the post-codegen steps of tools/codegen.sh again" >&2
  exit 1
fi
