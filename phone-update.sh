#!/data/data/com.termux/files/usr/bin/bash
# KyeSU self-update from the phone (Termux): rebase our commits onto the latest
# backslashxx upstream and force-push. Nothing is built here — launch the CI
# build yourself afterwards. Works under both bash and sh (dash/busybox).
#
# Requires (Termux): git, and push auth for origin (gh auth login, a PAT in the
# https URL, or an SSH remote). Do NOT `git fetch upstream` by hand first — just
# run this; and never a bare `git rebase upstream/master` (that replays the
# whole backslashxx stack and conflicts — this uses --onto instead).
#
#   ./phone-update.sh   (or: bash phone-update.sh)
set -eu
cd "$(dirname "$(readlink -f "$0")")"

git config rerere.enabled true
git remote get-url upstream >/dev/null 2>&1 || \
  git remote add upstream https://github.com/backslashxx/KernelSU.git
git fetch upstream --quiet
git fetch origin --quiet

# This tree always starts from what is on origin: main is force-pushed from
# both here and the desktop, so anything else means the other side pushed work
# this clone has never seen, and pushing over it would drop it.
if [ "$(git rev-parse main)" != "$(git rev-parse origin/main)" ]; then
  echo "!! main and origin/main differ — the desktop has pushed since the last sync"
  echo "   git reset --hard origin/main   # then re-run this script"
  exit 1
fi

# Base = backslashxx's own tip that our commits sit on (its version-bump commit,
# titled "KernelSU vX.Y.Z+"). Robust regardless of when upstream was fetched.
BASE="$(git log --format='%H %s' main | grep -m1 -E '^[0-9a-f]+ KernelSU v[0-9]' | cut -d' ' -f1 || true)"
NEW="$(git rev-parse upstream/master)"

if [ -z "$BASE" ]; then
  echo "!! could not find the backslashxx base commit in main — resolve on desktop"; exit 1
fi
if [ "$BASE" = "$NEW" ]; then
  echo "already on latest upstream ($NEW) — nothing to rebase"
else
  git branch -f _bak main
  if ! git rebase --onto upstream/master "$BASE" main; then
    git rebase --abort || true; git branch -D _bak || true
    echo "!! rebase conflict — resolve on desktop (build.sh), then push"; exit 1
  fi
  git branch -D _bak || true
  echo "rebased onto $NEW"
fi

git push --force origin main
echo "pushed — now run the CI build manually"
