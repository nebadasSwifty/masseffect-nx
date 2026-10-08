#!/usr/bin/env bash
# Build the code and the NRO of one game edition (language/build) from this single tree.
#
#   tools/edition.sh <id> [stage|codegen|build|all]   id: a folder name under editions/ (en, ru)   default step: all
#
# Environment:
#   EDITION_XEX   the default.xex of YOUR disc for this edition (default: assets/game_root/default.xex for en,
#                 assets/game_root-<id>/default.xex otherwise). Its SHA-256 must be one of editions/<id>/edition.env XEX_SHA256.
#   other variables of tools/codegen.sh and tools/build_nro.sh (MESA_SDK, JOBS, ...).
#
# English builds in this tree (as before). Any other edition is staged into out/edition-<id>/: a copy of app/ and tools/ with
# editions/<id>/overlay applied on top (the files tied to that default.xex's guest addresses: overrides, hook lists, native
# replacements, patch scripts), sdk/ and shaders/ linked, and an edition.env at its root; codegen and the NRO build then run there.
# The NRO is copied to out/nx-<id>/<NRO_NAME>.nro. The user's game data never leaves their machine.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ID="${1:?usage: tools/edition.sh <id> [stage|codegen|build|all]}"; STEP="${2:-all}"
ED="$ROOT/editions/$ID"
[[ -f "$ED/edition.env" ]] || { echo "error: unknown edition '$ID' (see editions/)" >&2; exit 1; }
. "$ED/edition.env"
if [[ "$ID" == en ]]; then XEX_DEFAULT="$ROOT/assets/game_root/default.xex"; else XEX_DEFAULT="$ROOT/assets/game_root-$ID/default.xex"; fi
XEX="${EDITION_XEX:-$XEX_DEFAULT}"

check_xex() {
  [[ -f "$XEX" ]] || { echo "error: $XEX not found: put your $EDITION_NAME default.xex there (or set EDITION_XEX)" >&2; exit 1; }
  local sha; sha="$(shasum -a 256 "$XEX" | cut -c1-64)"
  for known in $XEX_SHA256; do [[ "$sha" == "$known" ]] && { echo "== edition $ID: default.xex recognised ($sha)"; return; }; done
  echo "error: this default.xex (SHA-256 $sha) is not a known $EDITION_NAME file." >&2
  echo "       Known: $XEX_SHA256" >&2
  echo "       If it is the same build repacked, compare its XEX header (python3 tools/extract_iso.py FILE --info: entry point $XEX_ENTRY)" >&2
  echo "       and add the hash to editions/$ID/edition.env." >&2; exit 1
}

if [[ "$ID" == en ]]; then
  W="$ROOT"
else
  W="$ROOT/out/edition-$ID"
fi

stage() {
  [[ "$ID" == en ]] && return 0
  echo "== staging $W"
  # Overlay files replace English ones: show any change of the English file the overlay copy does not have.
  python3 "$ROOT/tools/edition_drift.py" "$ID" | sed 's/^/   drift: /'
  mkdir -p "$W/assets/game_root" "$W/out"
  rsync -a --delete --exclude generated --exclude '.manifest.extra.toml' "$ROOT/app/" "$W/app/"
  rsync -a --delete "$ROOT/tools/" "$W/tools/"
  if [[ -d "$ED/overlay" ]]; then
    # --checksum: an overlay file often has the same size as the English one (only same-length addresses differ) and,
    # when both were edited in the same second, the same mtime: rsync's quick check then keeps the English file.
    # No -t: a copied file gets the current time, so ninja recompiles it even when its object is newer than the
    # overlay's own (older) mtime. -a here kept a stale object of the English file in the build.
    rsync -rlpD --checksum "$ED/overlay/" "$W/"
    (cd "$ED/overlay" && find . -type f | while read -r f; do
      cmp -s "$f" "$W/$f" || { echo "error: overlay file $f was not applied to $W" >&2; exit 1; }
    done) || exit 1
  fi
  # Hooks every edition's app header must keep (an overlay copy without them silently disables the feature:
  # the RU full NSP failed with "--game_data_root was not provided" that way, docs/full-nsp.md).
  for hook in 'me::packaged::ConfigurePaths(paths)' 'me::packaged::ReportPaths(paths)' 'me::packaged::LogStatus()'; do
    grep -qF "$hook" "$W/app/src/masseffect_app.h" || {
      echo "error: $W/app/src/masseffect_app.h lacks $hook (update the overlay copy)" >&2; exit 1; }
  done
  cp "$ED/edition.env" "$W/edition.env"
  # Docker only mounts the tree itself: small inputs the build reads are copied, not linked (sdk is mounted by build_nro.sh).
  rsync -a "$ROOT/extras/" "$W/extras/"
  for d in sdk shaders mesa installer; do if [[ -e "$ROOT/$d" ]]; then ln -sfn "$ROOT/$d" "$W/$d"; fi; done
  mkdir -p "$W/out"
  if [[ -e "$ROOT/out/mesa-sdk" && ! -e "$W/out/mesa-sdk" ]]; then ln -sfn "$ROOT/out/mesa-sdk" "$W/out/mesa-sdk"; fi
  return 0
}

case "$STEP" in
  stage) stage ;;
  codegen|build|all)
    check_xex; stage
    export XEX
    if [[ "$ID" != en && ! "$XEX" -ef "$W/assets/game_root/default.xex" ]]; then cp -f "$XEX" "$W/assets/game_root/default.xex"; fi
    # Reading the virtual counter (mrs ..., cntvct_el0) traps on Horizon and kills the calling thread (2026-10-07: the
    # ring thread died at start, black screen). Use cntpct_el0 / armGetSystemTick.
    if grep -rnE 'mrs %[^,]*, *cntvct_el0' "$W/app/src" "$ROOT/sdk/src" >/dev/null 2>&1; then
      echo "error: cntvct_el0 read in the sources (traps on Horizon):" >&2; grep -rnE 'mrs %[^,]*, *cntvct_el0' "$W/app/src" "$ROOT/sdk/src" >&2; exit 1; fi
    # Two REX_HOOK_RAW definitions of one guest function link without an error and one of them is silently lost.
    DUP_HOOKS="$(grep -rhoE 'REX_HOOK_RAW\(sub_[0-9A-Fa-f]+\)' "$W/app/src" | sort | uniq -d)"
    if [[ -n "$DUP_HOOKS" ]]; then echo "error: guest functions hooked more than once: $DUP_HOOKS" >&2; exit 1; fi
    # The post-codegen direct-call patch bakes in the set of hooked guest functions (every 82xxxxxx address in the sources).
    # The generator keeps unchanged output, so after the hook set changes (new overlay file, new hook) the old direct calls
    # would bypass the new hooks. Regenerate from scratch when the set differs from the one of the last codegen.
    HOOKS_NOW="$(cd "$W" && python3 -c 'import importlib.util as u;s=u.spec_from_file_location("d","tools/direct_calls.py");m=u.module_from_spec(s);s.loader.exec_module(m);print("\n".join(sorted(m.hooked())))' | shasum -a 256 | cut -c1-64)"
    HOOKS_FILE="$W/app/generated/default/.hooked_set.sha256"
    if [[ -d "$W/app/generated/default" && "$(cat "$HOOKS_FILE" 2>/dev/null)" != "$HOOKS_NOW" ]]; then
      if [[ "$STEP" == build ]]; then echo "error: hooked functions changed since the last codegen: run tools/edition.sh $ID codegen (or all)" >&2; exit 1; fi
      echo "== hooked function set changed since the last codegen: regenerating from scratch"; rm -rf "$W/app/generated"
    fi
    if [[ "$STEP" != build ]]; then ( cd "$W" && XEX="$W/assets/game_root/default.xex" bash tools/codegen.sh ); echo "$HOOKS_NOW" > "$HOOKS_FILE"; fi
    if [[ "$STEP" != codegen ]]; then
      # Default driver: out/mesa-sdk (-O2); MESA_OPT=O1 picks the -O1 fallback out/mesa-sdk-o1 (docs/mesa.md).
      MESA_DIR_NAME=mesa-sdk; if [[ "${MESA_OPT:-}" =~ ^[Oo]?1$ ]]; then MESA_DIR_NAME=mesa-sdk-o1; fi
      ( cd "$W" && MESA_SDK="${MESA_SDK:-$( [[ -d "$ROOT/out/$MESA_DIR_NAME" ]] && echo "$ROOT/out/$MESA_DIR_NAME" || echo "$ROOT/../$MESA_DIR_NAME" )}" bash tools/build_nro.sh )
      if [[ "$ID" != en ]]; then
        mkdir -p "$ROOT/out/nx-$ID"; cp -f "$W/out/nx/masseffect-nx.nro" "$ROOT/out/nx-$ID/$NRO_NAME.nro" 2>/dev/null \
          && echo "== NRO: $ROOT/out/nx-$ID/$NRO_NAME.nro"
      fi
    fi ;;
  *) echo "unknown step '$STEP'" >&2; exit 1 ;;
esac
