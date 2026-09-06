#!/data/data/com.termux/files/usr/bin/bash
# KyeSU self-update from the phone (Termux): rebase onto latest backslashxx
# upstream and force-push. Nothing is built or installed here — trigger the
# GitHub CI build yourself afterwards.
#
# Requires (Termux): git. Run `pkg install git` if missing.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

git config rerere.enabled true
git remote get-url upstream >/dev/null 2>&1 || \
  git remote add upstream https://github.com/backslashxx/KernelSU.git

OLD="$(git rev-parse upstream/master 2>/dev/null || echo none)"
git fetch upstream --quiet
NEW="$(git rev-parse upstream/master)"

if [ "$OLD" = "$NEW" ] || [ "$OLD" = none ]; then
  echo "upstream unchanged ($NEW) — nothing to rebase"
else
  git branch -f _bak main
  if ! git rebase --onto upstream/master "$OLD" main; then
    git rebase --abort || true; git branch -D _bak || true
    echo "!! rebase conflict — resolve on desktop (build.sh), then push"; exit 1
  fi
  git branch -D _bak || true
  echo "rebased onto $NEW"
fi

git push --force origin main
echo "pushed — now run the CI build manually"
