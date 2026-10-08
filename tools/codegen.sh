#!/usr/bin/env bash
# Translates the game (default.xex) to C++ with the host code generator, then applies and checks the post-codegen
# patches the build depends on. It builds nothing: build the generator first with tools/build_host.sh.
#
#   tools/codegen.sh
#
# Input:   assets/game_root/default.xex   (tools/extract_iso.py puts it there)
#          app/masseffect_manifest.toml   (project, game paths, includes: overrides.toml, perf_overrides.toml)
# Output:  app/generated/                 (C++ of the game and generated/rexglue.cmake; not committed)
#          out/codegen.log                (log of the generator)
#
# Environment (all optional):
#   REXGLUE            the rexglue executable (default sdk/out/host/rexglue)
#   PYTHON             Python interpreter (default python3)
#   ARGS_IN_REGISTERS  0 or 1: overrides `args_in_registers` of app/perf_overrides.toml for this run. The pass lives in
#                      the generator; with a generator that lacks it, tools/args_in_registers.py applies the same
#                      transformation to the generated code afterwards.
#   EXTRA_TOMLS        space-separated toml files (names in app/ or absolute paths) merged after
#                      perf_overrides.toml through a generated manifest (app/.manifest.extra.toml), for experiments.
#   STRICT             1: the register-use checks at the end fail the run instead of printing a warning.
#   ORDER_LD           linker script with the hot-function order (default app/function_order.ld); only touched
#                      when args_in_registers is on.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Edition constants (guest addresses of one default.xex): a staged edition tree (tools/edition.sh) has an edition.env at its root;
# without one the English values below apply.
[[ -f "$ROOT/edition.env" ]] && . "$ROOT/edition.env"
NORETURN="${NORETURN:-82ACA550}"
RESIDUE_VOLATILE="${RESIDUE_VOLATILE:-sub_82ACA670 sub_826710B0 sub_827F6558 sub_82BE7694}"
RESIDUE_ARGUMENT="${RESIDUE_ARGUMENT:-sub_826710B0 sub_827F6558}"
APP="$ROOT/app"
PY="${PYTHON:-python3}"
REXGLUE="${REXGLUE:-$ROOT/sdk/out/host/rexglue}"
MANIFEST_NAME=masseffect_manifest.toml
GEN="$APP/generated/default"
ORDER_LD="${ORDER_LD:-$APP/function_order.ld}"
export REXSDK_DIR="${REXSDK_DIR:-$ROOT/sdk}"      # SDK sources the hook scan reads (tools/direct_calls.py)

[[ -x "$REXGLUE" ]] || { echo "error: $REXGLUE not found; run tools/build_host.sh (or set REXGLUE)" >&2; exit 1; }
[[ -f "$APP/$MANIFEST_NAME" ]] || { echo "error: $APP/$MANIFEST_NAME not found" >&2; exit 1; }
XEX="${XEX:-$ROOT/assets/game_root/default.xex}"
[[ -f "$XEX" ]] || { echo "error: $XEX not found; extract your disc with tools/extract_iso.py first" >&2; exit 1; }

# Arguments in registers: the key of perf_overrides.toml, or ARGS_IN_REGISTERS for one run.
ARGS="${ARGS_IN_REGISTERS:-$(sed -nE 's/^args_in_registers *= *(true|false).*/\1/p' "$APP/perf_overrides.toml" | head -1)}"
case "$ARGS" in true|1) ARGS=1 ;; *) ARGS=0 ;; esac
export REX_ARGS_IN_REGISTERS=$ARGS                # overrides the toml key inside the generator
if [[ $ARGS == 1 ]]; then
  # Functions a hook may replace keep the old ABI: the generator reads this list (args_in_registers_exclude_file).
  "$PY" "$ROOT/tools/args_in_registers.py" --write-hooked "$APP/hooked_functions.txt"
fi

# Optional extra toml files after perf_overrides.toml.
MANIFEST="$MANIFEST_NAME"
if [[ -n "${EXTRA_TOMLS:-}" ]]; then
  MANIFEST=.manifest.extra.toml
  includes='"overrides.toml", "perf_overrides.toml"'
  for t in $EXTRA_TOMLS; do includes="$includes, \"$t\""; done
  sed "s#^includes = .*#includes = [$includes]#" "$APP/$MANIFEST_NAME" > "$APP/$MANIFEST"
  trap 'rm -f "$APP/$MANIFEST"' EXIT
fi

mkdir -p "$ROOT/out"
echo "== codegen ($APP/$MANIFEST, args_in_registers=$ARGS)"
if ! ( cd "$APP" && REX_MAX_JUMP_TABLE_ENTRIES=1024 REX_REGISTER_SHARD_SPAN_BYTES=262144 \
         "$REXGLUE" codegen "$MANIFEST" ) 2>&1 | tee "$ROOT/out/codegen.log"; then
  echo "codegen failed, see out/codegen.log" >&2
  exit 1
fi
[[ -d "$GEN" ]] || { echo "error: the generator produced no $GEN" >&2; exit 1; }

echo "== post-codegen patches"
# A new generation silently loses these. Each is idempotent; verify_pch.sh fails the run if one did not take effect.
"$PY" "$ROOT/tools/direct_calls.py"
"$PY" "$ROOT/tools/pch_no_volatile.py"
"$PY" "$ROOT/tools/pch_no_global_lock.py"
"$PY" "$ROOT/tools/pch_ui_viewport.py"
"$PY" "$ROOT/tools/pch_ui_world_to_screen.py"
"$PY" "$ROOT/tools/pch_indirect_dispatch.py"
"$ROOT/tools/verify_pch.sh" "$GEN"

if [[ $ARGS == 1 ]]; then
  if grep -q '__fast_sub_' "$GEN"/masseffect_recomp.*.cpp; then
    echo "args_in_registers: applied by the generator"
  else
    echo "args_in_registers: this generator lacks the option, applying tools/args_in_registers.py"
    "$PY" "$ROOT/tools/args_in_registers.py" --gen "$GEN" --apply
  fi
  # hot-function sections of the fast entries
  if [[ -f "$ORDER_LD" ]]; then "$PY" "$ROOT/tools/args_in_registers.py" --fix-ordering "$ORDER_LD"
  else echo "warning: $ORDER_LD not found, hot-function order not updated" >&2; fi
fi

echo "== register-use checks (tools/read_before_write.py)"
# non_volatile_as_local (app/perf_overrides.toml): only functions that are known to be harmless may read a register
# before writing it. The guest longjmp 0x82ACA550 is treated as noreturn. Any other name means a split piece or
# funclet that needs `share_registers` in perf_overrides.toml (a crash at startup otherwise).
check_residue() {  # description allowed-names... -- extra read_before_write.py arguments...
  local what="$1"; shift
  local allowed=()
  while [[ $# -gt 0 && "$1" != "--" ]]; do allowed+=("$1"); shift; done
  shift
  local out unexpected=""
  out="$("$PY" "$ROOT/tools/read_before_write.py" "$GEN" --noreturn=$NORETURN "$@" 2>/dev/null)"
  while read -r name _; do
    [[ "$name" == sub_* ]] || continue
    local ok=0 a
    for a in ${allowed[@]+"${allowed[@]}"}; do [[ "$a" == "$name" ]] && ok=1; done
    [[ $ok -eq 1 ]] || unexpected="$unexpected $name"
  done <<< "$out"
  if [[ -n "$unexpected" ]]; then
    local label=warning; [[ "${STRICT:-0}" == 1 ]] && label=error
    echo "$label: $what: unexpected functions read a register before writing it:$unexpected" >&2
    echo "  see the comment on share_registers in app/perf_overrides.toml; run tools/read_before_write.py for details" >&2
    [[ "${STRICT:-0}" == 1 ]] && exit 1
  else
    echo "$what: ok"
  fi
}
# Known residue (harmless): guest setjmp 0x82ACA670, two jump-table artifacts, one 4-byte orphan fragment.
check_residue "non_volatile_as_local" $RESIDUE_VOLATILE --
# non_argument_as_local / reserved_as_local: only the two jump-table artifacts.
check_residue "non_argument_as_local" $RESIDUE_ARGUMENT -- --volatile

echo "done: $GEN"
