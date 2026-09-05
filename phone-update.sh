#!/data/data/com.termux/files/usr/bin/bash
# KyeSU self-update from the phone (Termux).
# Rebases onto latest upstream, pushes, then lets GitHub CI build the LKM +
# ksud + signed manager APK, downloads it, and installs it. The heavy build
# runs in CI because the LKM (DDK container) and the Android/Gradle build are
# not practical on-device.
#
# Requires (Termux): git, gh (authenticated: `gh auth login`). Root optional
# (used for silent install; otherwise the installer UI is opened).
#
# Usage:
#   ./phone-update.sh            # sync + push + CI build + install
#   ./phone-update.sh --no-sync  # skip rebase/push, just build current main + install
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

WF="build-manager.yml"
SYNC=1; [ "${1:-}" = "--no-sync" ] && SYNC=0
say(){ printf '\n== %s\n' "$*"; }
need(){ command -v "$1" >/dev/null || { echo "missing: $1 (pkg install $1)"; exit 1; }; }
need git; need gh

# ---- 1. sync onto upstream + push ----
if [ "$SYNC" = 1 ]; then
  say "sync onto upstream/master"
  git config rerere.enabled true
  git remote get-url upstream >/dev/null 2>&1 || \
    git remote add upstream https://github.com/backslashxx/KernelSU.git
  OLD="$(git rev-parse upstream/master 2>/dev/null || echo none)"
  git fetch upstream --quiet
  NEW="$(git rev-parse upstream/master)"
  if [ "$OLD" != "$NEW" ] && [ "$OLD" != none ]; then
    git branch -f _bak main
    if ! git rebase --onto upstream/master "$OLD" main; then
      git rebase --abort || true; git branch -D _bak || true
      echo "!! rebase conflict — resolve on desktop (build.sh), then re-run with --no-sync"; exit 1
    fi
  fi
  git push --force origin main
fi

# ---- 2. trigger CI build ----
say "dispatch CI ($WF)"
gh workflow run "$WF" --ref main
sleep 8
RID="$(gh run list --workflow "$WF" --branch main -L1 --json databaseId -q '.[0].databaseId')"
echo "run: $RID"

# ---- 3. wait for it ----
say "waiting for CI (~20 min)"
gh run watch "$RID" --exit-status

# ---- 4. download the signed, repacked manager APK ----
say "download artifact"
OUT="$(mktemp -d)"
gh run download "$RID" -n manager -D "$OUT" 2>/dev/null \
  || gh run download "$RID" -n manager-gradle -D "$OUT"
APK="$(find "$OUT" -name '*.apk' | head -1)"
[ -n "$APK" ] || { echo "no apk in artifact"; exit 1; }
echo "APK: $APK"

# ---- 5. install ----
say "install"
if command -v su >/dev/null && su -c 'id' >/dev/null 2>&1; then
  su -c "pm install -r '$APK'" && echo "installed"
else
  echo "no root — opening installer"; termux-open "$APK" 2>/dev/null || echo "open manually: $APK"
fi
say "done"
