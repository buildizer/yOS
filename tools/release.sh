#!/bin/zsh
# Publish a yOS update that boards can install from Settings › General › About.
#   tools/release.sh 1.2 "What's new in this version"
# Sets the version, builds, commits and pushes, then creates the GitHub Release v<version>
# (tag, firmware.bin.gz and firmware.bin together). Boards signed in to GitHub see it on their next check;
# yOS 1.3 and later download firmware.bin.gz (a third smaller), older boards firmware.bin.
set -e
cd "$(dirname $0)/.."
VERSION=$1
NOTES=${2:-}
[[ $VERSION =~ '^[0-9]+(\.[0-9]+)*$' ]] || { echo "usage: tools/release.sh <version, e.g. 1.2> [notes]" >&2; exit 2; }
[[ -z $(git status --porcelain) ]] || { echo "Commit or stash your changes first." >&2; exit 1; }
# A build without the app's client ID can't sign in, so boards that install it lose GitHub updates.
grep -q 'GITHUB_APP_CLIENT_ID *"[^"]\+"' include/config.h \
  || { echo "Set GITHUB_APP_CLIENT_ID in include/config.h first." >&2; exit 1; }
if git rev-parse -q --verify "refs/tags/v$VERSION" >/dev/null || [[ -n $(git ls-remote --tags origin "v$VERSION") ]]; then
  echo "v$VERSION already exists." >&2; exit 1
fi

sed -i '' "s/^#define YOS_FIRMWARE_VERSION \".*\"/#define YOS_FIRMWARE_VERSION \"$VERSION\"/" include/version.h
trap 'git checkout -q include/version.h' ERR  # a failed build leaves the tree as it was
pio run -e yos
git commit -q -m "build: Release yOS $VERSION" include/version.h
trap - ERR
git push -q origin HEAD

# gh creates the tag on the pushed commit together with the release, so a failure here leaves no
# orphan tag and the script can simply be run again.
if [[ -n $NOTES ]]; then NOTE_ARGS=(--notes "$NOTES"); else NOTE_ARGS=(--generate-notes); fi
gh release create "v$VERSION" .pio/build/yos/firmware.bin.gz .pio/build/yos/firmware.bin --target "$(git rev-parse HEAD)" \
  --title "yOS $VERSION" $NOTE_ARGS
git fetch -q --tags origin
echo "Released yOS $VERSION"
