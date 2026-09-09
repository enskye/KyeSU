#!/usr/bin/env bash
# KyeSU local build pipeline (desktop / Linux).
# Sync onto latest backslashxx upstream, build LKM + ksud + manager, install to phone.
#
# Usage:
#   ./build.sh              # sync + build + install (no push)
#   ./build.sh --push       # also force-push the rebased main to origin
#   ./build.sh --no-sync    # skip the upstream rebase, just build what's here
#   ./build.sh --no-install # build only, don't adb install
#   ./build.sh --skip-lkm   # reuse the existing bundled .ko (no podman rebuild)
#
# Config via env (sensible defaults for this machine):
#   ANDROID_NDK_HOME, ANDROID_HOME, DDK_IMAGE, KMI, KS_PASS, KS_ALIAS
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

# ---- config ----
KMI="${KMI:-android16-6.12}"
DDK_IMAGE="${DDK_IMAGE:-ghcr.io/ylarod/ddk-min:${KMI}-20260828}"
ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
NDK="${ANDROID_NDK_HOME:-$(ls -d "$HOME"/Android/Sdk/ndk/* 2>/dev/null | sort -V | tail -1)}"
[ -d "$NDK" ] || NDK="$(ls -d "$HOME"/Projects/VPN/android-sdk/ndk/* 2>/dev/null | sort -V | tail -1)"
KS="$PWD/manager/kyesu.keystore"; KS_PASS="${KS_PASS:-password}"; KS_ALIAS="${KS_ALIAS:-kyesu}"
STRIP="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
export PATH="$HOME/.cargo/bin:$PATH"

SYNC=1 INSTALL=1 PUSH=0 LKM=1
for a in "$@"; do case "$a" in
  --no-sync) SYNC=0;; --no-install) INSTALL=0;; --push) PUSH=1;; --skip-lkm) LKM=0;;
  *) echo "unknown arg: $a"; exit 2;; esac; done

# Rebase, letting scripts/rebase-resolve.sh handle the recurring conflicts.
rebase_onto() {
  git rebase --onto upstream/master "$1" main && return 0
  while [ -d .git/rebase-merge ] || [ -d .git/rebase-apply ]; do
    if ! sh scripts/rebase-resolve.sh; then
      return 1
    fi
    if git diff --cached --quiet; then
      GIT_EDITOR=true git rebase --skip || return 1
    else
      GIT_EDITOR=true git rebase --continue || return 1
    fi
  done
  return 0
}

say(){ printf '\n\033[1;36m== %s\033[0m\n' "$*"; }

# ---- 1. sync onto latest upstream ----
if [ "$SYNC" = 1 ]; then
  say "sync onto upstream/master"
  git config rerere.enabled true
  git fetch upstream --quiet
  # Base = backslashxx's own tip our commits sit on (its "KernelSU vX.Y.Z+"
  # commit). Robust regardless of when upstream was last fetched.
  BASE="$(git log --format='%H %s' main | grep -m1 -E '^[0-9a-f]+ KernelSU v[0-9]' | cut -d' ' -f1 || true)"
  NEW="$(git rev-parse upstream/master)"
  [ -n "$BASE" ] || { echo "!! could not find backslashxx base in main"; exit 1; }
  if [ "$BASE" != "$NEW" ]; then
    git branch -f _bak main
    if ! rebase_onto "$BASE"; then
      echo "!! unresolved rebase conflict — fix it, git rebase --continue, then re-run with --no-sync"; exit 1
    fi
    echo "rebased onto $NEW (backup: _bak)"
  else
    echo "upstream unchanged ($NEW)"
  fi
fi

# ---- 2. LKM (kernelsu.ko) via DDK container ----
if [ "$LKM" = 1 ]; then
  say "build LKM ($KMI) in $DDK_IMAGE"
  podman run --rm --network none -v "$PWD":/ksu:Z -w /ksu/kernel "$DDK_IMAGE" \
    bash -c 'git config --global --add safe.directory "*"; CONFIG_KSU=m CC=clang make >/dev/null'
  cp -f kernel/ksu.ko "userspace/ksud/bin/aarch64/${KMI}_kernelsu.ko"
  "$STRIP" -d "userspace/ksud/bin/aarch64/${KMI}_kernelsu.ko"
  find kernel -maxdepth 1 \( -name '*.o' -o -name '*.cmd' -o -name '*.ko' -o -name '*.mod*' \
    -o -name 'Module.symvers' -o -name 'modules.order' \) -delete 2>/dev/null || true
fi

# ---- 3. ksuinit (only if missing; source rarely changes) ----
if [ ! -f userspace/ksud/bin/aarch64/ksuinit ]; then
  say "build ksuinit"
  ( cd . ; ANDROID_NDK_HOME="$NDK" bash -c '
      source .github/scripts/setup-rust-build.sh aarch64-linux-android 26
      BUILTINS="$($CLANG_PATH --print-resource-dir)/lib/linux/libclang_rt.builtins-aarch64-android.a"
      RUSTFLAGS="-C target-feature=+crt-static -C link-arg=-Wl,-z,max-page-size=16384 -C link-arg=-Wno-unused-command-line-argument -C link-arg=$BUILTINS" \
        cargo build --package ksuinit --target=aarch64-linux-android --release' )
  cp -f target/aarch64-linux-android/release/ksuinit userspace/ksud/bin/aarch64/ksuinit
fi

# ---- 4. ksud (embeds LKM + ksuinit) ----
say "build ksud"
( cd userspace/ksud && unset RUSTFLAGS && ANDROID_NDK_HOME="$NDK" cargo ndk -t arm64-v8a build --release )
cp -f target/aarch64-linux-android/release/ksud manager/app/src/main/jniLibs/arm64-v8a/libksud.so

# ---- 5. manager APK (signed with kyesu key) ----
say "build manager"
( cd manager && ANDROID_HOME="$ANDROID_HOME" ./gradlew clean assembleRelease \
    -PKEYSTORE_FILE="$KS" -PKEYSTORE_PASSWORD="$KS_PASS" -PKEY_ALIAS="$KS_ALIAS" -PKEY_PASSWORD="$KS_PASS" )
APK="$(find manager/app/build/outputs/apk/release -name '*.apk' | head -1)"
echo "APK: $APK"

# ---- 6. install ----
if [ "$INSTALL" = 1 ]; then
  say "install to phone"
  adb install -r "$APK"
  adb shell dumpsys package lt.kye.ksu 2>/dev/null | grep -iE 'versionCode' | head -1 || true
fi

# ---- 7. optional push ----
if [ "$PUSH" = 1 ]; then
  say "push"
  git log main ^upstream/master --format='%b' | grep -qi 'co-authored\|claude' \
    && { echo '!! refusing: claude trailer found'; exit 1; } || true
  git push --force origin main
fi

git checkout -- Cargo.lock 2>/dev/null || true
say "done"
