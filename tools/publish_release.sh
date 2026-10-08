#!/usr/bin/env bash
# Upload a locally built NRO as a draft, then let Actions pack NSP and publish.
# Usage: tools/publish_release.sh v0.1.0 [out/nx/masseffect-nx.nro]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
TAG=${1:?usage: publish_release.sh v0.1.0 [file.nro]}
NRO=${2:-out/nx/masseffect-nx.nro}
[[ "$TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+([-+][a-zA-Z0-9.-]+)?$ ]] || { echo 'error: expected a version tag vX.Y.Z' >&2; exit 1; }
[[ -f "$NRO" ]] || { echo "error: missing $NRO" >&2; exit 1; }
[[ -z "$(git status --porcelain)" ]] || { echo 'error: commit all source changes before releasing' >&2; exit 1; }
git rev-parse "$TAG" >/dev/null 2>&1 || git tag "$TAG"
[[ "$(git rev-parse "$TAG^{commit}")" = "$(git rev-parse HEAD)" ]] || { echo 'error: tag does not match the current sources' >&2; exit 1; }
git push origin HEAD:main "$TAG"
if ! gh release view "$TAG" >/dev/null 2>&1; then
  gh release create "$TAG" --verify-tag --draft --title "Mass Effect NX $TAG" --notes-file docs/release-notes.md
fi
gh release upload "$TAG" "$NRO#masseffect-nx.nro" --clobber
# The editions' shipped pipeline prewarm lists (installer/config.js editions[].prewarmList); optional.
for list in app/prewarm/masseffect_prewarm_list-*.bin; do
  [[ ! -f "$list" ]] || gh release upload "$TAG" "$list" --clobber
done
gh workflow run release.yml --ref main -f tag="$TAG" -f prerelease=false
echo "Actions will build the NSP and publish: https://github.com/nebadasSwifty/masseffect-nx/actions"
