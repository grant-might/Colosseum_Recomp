#!/usr/bin/env bash
# Fetch the pinned upstreams and copy this repo's overlay over them.
# This does not build anything and does not touch any remote.
#
# Usage:
#   ./bootstrap.sh                 fetch into ./work
#   WORK=/some/dir ./bootstrap.sh  fetch somewhere else
#   ./bootstrap.sh --with-dolphin  also fetch the Dolphin submodule (about 880 MB)

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${WORK:-$HERE/work}"

TEMPLATE_URL="https://github.com/ExpansionPak/ModernGekko-Template.git"
MODERNGEKKO_URL="https://github.com/ExpansionPak/ModernGekko.git"
DOLRECOMP_URL="https://github.com/ExpansionPak/DolRecomp.git"

TEMPLATE_COMMIT="c2553a99584b2045dbe46a70e1570994dae8beef"
MODERNGEKKO_COMMIT="edd460adad0e82b56909af1d793642c83b6d31c8"
DOLRECOMP_COMMIT="1bec3554ecc4817cf78319ca3d8a0669477f29fa"

WITH_DOLPHIN=0
for arg in "$@"; do
  case "$arg" in
    --with-dolphin) WITH_DOLPHIN=1 ;;
    -h|--help) sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $arg" >&2; exit 1 ;;
  esac
done

command -v git >/dev/null || { echo "git is required" >&2; exit 1; }

# Clone at a commit, no branch tracking. Full clone, then detach: the pinned
# commits are not necessarily reachable from a shallow default branch.
fetch_repo() {
  local url="$1" commit="$2" dest="$3"
  if [ -d "$dest/.git" ]; then
    echo "already present: $dest"
  else
    mkdir -p "$(dirname "$dest")"
    echo "cloning $url"
    git clone --quiet "$url" "$dest"
  fi
  echo "checking out $commit in $dest"
  git -C "$dest" fetch --quiet origin "$commit" 2>/dev/null || true
  git -C "$dest" -c advice.detachedHead=false checkout --quiet "$commit"
}

echo "work dir: $WORK"
mkdir -p "$WORK"

# 1. Outer tree. Submodules are fetched separately below so the commits are the
#    ones in versions.txt rather than whatever the outer tree happens to record.
fetch_repo "$TEMPLATE_URL" "$TEMPLATE_COMMIT" "$WORK/ModernGekko-Template"

# 2. ModernGekko at the pinned commit, into the submodule path.
fetch_repo "$MODERNGEKKO_URL" "$MODERNGEKKO_COMMIT" "$WORK/ModernGekko-Template/lib/ModernGekko"

# 3. DolRecomp at the pinned commit, into the other submodule path.
fetch_repo "$DOLRECOMP_URL" "$DOLRECOMP_COMMIT" "$WORK/ModernGekko-Template/lib/DolRecomp"

if [ "$WITH_DOLPHIN" = "1" ]; then
  echo "fetching vendor/dolphin (this is the large one)"
  git -C "$WORK/ModernGekko-Template/lib/ModernGekko" submodule update --init --depth 1 vendor/dolphin
else
  echo "skipped vendor/dolphin. The build needs it, see versions.txt."
fi

# 4. Overlay. Paths in overlay/ are rooted at the template tree, so this is a
#    straight copy into $WORK.
echo "applying overlay"
cp -R "$HERE/overlay/." "$WORK/"

echo
echo "done"
echo "  template   $WORK/ModernGekko-Template        $TEMPLATE_COMMIT"
echo "  moderngekko $WORK/ModernGekko-Template/lib/ModernGekko  $MODERNGEKKO_COMMIT"
echo "  dolrecomp  $WORK/ModernGekko-Template/lib/DolRecomp     $DOLRECOMP_COMMIT"
echo
echo "files replaced by the overlay:"
echo "  Makefile"
echo "  lib/ModernGekko/CMakeLists.txt"
echo "  lib/ModernGekko/tools/moderngekko_launcher.cpp"
echo "  lib/ModernGekko/tools/moderngekko_run.cpp"
echo "  lib/ModernGekko/tools/frontend_config.cpp"
echo
echo "lib/ModernGekko is now a dirty submodule. That is expected."
echo "Nothing was pushed and nothing was committed."
