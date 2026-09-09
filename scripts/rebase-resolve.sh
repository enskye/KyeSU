#!/bin/sh
# Auto-resolve the recurring conflicts of rebasing our commits onto backslashxx.
#
# Called from build.sh / phone-update.sh while a rebase is stopped. During
# `git rebase --onto upstream/master`, the "HEAD" side of a conflict is the new
# upstream and the other side is our commit, so the rule is almost always
# "keep our side" plus a value picked out of the upstream side.
#
# Exit 0 = every conflicted file was resolved and staged, 1 = needs a human.
set -eu
cd "$(git rev-parse --show-toplevel)"

# keep the lower (our-commit) half of every conflict block in $1
take_ours() {
	awk '
	/^<<<<<<< /	{ c = 1; keep = 0; next }
	/^=======$/	{ if (c) { keep = 1; next } }
	/^>>>>>>> /	{ if (c) { c = 0; keep = 0; next } }
			{ if (!c || keep) print }
	' "$1" > "$1.resolved" && mv "$1.resolved" "$1"
}

# keep the upper (upstream) half of every conflict block in $1
take_upstream() {
	awk '
	/^<<<<<<< /	{ c = 1; keep = 1; next }
	/^=======$/	{ if (c) { keep = 0; next } }
	/^>>>>>>> /	{ if (c) { c = 0; next } }
			{ if (!c || keep) print }
	' "$1" > "$1.resolved" && mv "$1.resolved" "$1"
}

# text of the upstream half of every conflict block in $1
upstream_side() {
	awk '/^<<<<<<< /{ c = 1; next } /^=======$/{ c = 0 } c' "$1"
}

rc=0
for f in $(git diff --name-only --diff-filter=U); do
	case "$f" in
	kernel/Makefile)
		# upstream hand-bumps -DKSU_VERSION=NNNNN and keeps reshaping the
		# signature block next to it, so our half of a conflict goes stale.
		# Take upstream's file and re-apply what the replayed commit adds: the
		# commit-count version block (upstream's number becomes its no-git
		# fallback), our manager package name and signing key values. rerere may have already
		# replayed an older resolution, so start again from the conflict.
		git checkout -m -- "$f"
		take_upstream "$f"
		ours="$(git show "REBASE_HEAD:$f")"
		prev="$(git show "REBASE_HEAD^:$f")"
		v="$(sed -n 's/^CFLAGS_ksu.o += -DKSU_VERSION=\([0-9][0-9]*\)$/\1/p' "$f" | head -1)"
		if printf '%s\n' "$ours" | grep -q '^KSU_GIT_ROOT :=' &&
		   ! printf '%s\n' "$prev" | grep -q '^KSU_GIT_ROOT :=' &&
		   ! grep -q '^KSU_GIT_ROOT :=' "$f"; then
			printf '%s\n' "$ours" | awk '
			/^# version must match getVersionCode/ { p = 1 }
			p { print }
			/^CFLAGS_ksu.o \+= -DKSU_VERSION=\$\(KSU_VERSION\)$/ { exit }
			' > "$f.block"
			awk -v blk="$f.block" '
			/^# compliant to last upstream kernel change/ { next }
			/^CFLAGS_ksu.o \+= -DKSU_VERSION=[0-9]+$/ { while ((getline l < blk) > 0) print l; next }
			{ print }
			' "$f" > "$f.resolved" && mv "$f.resolved" "$f"
			rm -f "$f.block"
		fi
		if [ -n "$v" ]; then
			sed -i "s/^KSU_VERSION := [0-9][0-9]*\$/KSU_VERSION := $v/" "$f"
		fi
		for k in KSU_PACKAGE_NAME KSU_EXPECTED_SIZE KSU_EXPECTED_HASH; do
			val="$(printf '%s\n' "$ours" | sed -n "s/^$k := //p" | head -1)"
			pval="$(printf '%s\n' "$prev" | sed -n "s/^$k := //p" | head -1)"
			if [ -n "$val" ] && [ "$val" != "$pval" ]; then
				sed -i "s/^$k := .*/$k := $val/" "$f"
			fi
		done
		;;
	manager/build.gradle.kts|userspace/ksud/build.rs)
		# upstream re-tunes the offset it subtracts from the commit count to
		# line up with official numbering; we use the plain count everywhere,
		# so take whatever else upstream changed there and drop the offset.
		take_upstream "$f"
		sed -i -e 's/\(return 30000 + commitCount\) - [0-9][0-9]*/\1/' \
		       -e 's/\(let version_code = 30000 + version_code\) - [0-9][0-9]*;/\1;/' "$f"
		# ksud's default manager package name: keep ours (see manager/gradle.properties)
		if [ "$f" = userspace/ksud/build.rs ]; then
			pkgof() { git show "$1:$f" | sed -n 's/.*cargo:rustc-env=KSU_PACKAGE_NAME=\([^"]*\)".*/\1/p' | head -1; }
			pkg="$(pkgof REBASE_HEAD)"
			if [ -n "$pkg" ] && [ "$pkg" != "$(pkgof REBASE_HEAD^)" ]; then
				sed -i "s/\(cargo:rustc-env=KSU_PACKAGE_NAME=\)[^\"]*\"/\1$pkg\"/" "$f"
			fi
		fi
		;;
	.github/workflows/*)
		# our CI policy: manual dispatch only, and the website / extra-target
		# workflows are deleted. Upstream keeps re-adding push/PR triggers and
		# editing files we removed, so keep our side -- a file we deleted stays
		# deleted, everything else takes our half of each conflicting hunk.
		# modify/delete: the commit being replayed dropped the file, while
		# upstream edited it. git leaves upstream's copy in the tree, so ask
		# the replayed commit instead and honour its deletion.
		if ! git cat-file -e "REBASE_HEAD:$f" 2>/dev/null; then
			git rm -q -f --ignore-unmatch "$f"
			continue
		fi
		take_ours "$f"
		;;
	kernel/manager/apk_sign.c)
		# upstream keeps adding fallback signing keys and reworking the code
		# around them; we accept only ours. Take upstream's file, then put back
		# our certificate size limit and an is_manager_apk() that checks just
		# EXPECTED_SIZE/EXPECTED_HASH (the package name is pinned elsewhere).
		git checkout -m -- "$f"
		take_upstream "$f"
		if git show "REBASE_HEAD:$f" | grep -q 'KyeSU manager signing key only'; then
			sed -i 's/^#define CERT_MAX_LENGTH [0-9]*$/#define CERT_MAX_LENGTH 2048/' "$f"
			awk '
			/^bool is_manager_apk\(char \*path\)$/ {
				print; print "{"
				print "\t// KyeSU manager signing key only"
				print "\tif (check_v2_signature(path, EXPECTED_SIZE, EXPECTED_HASH))"
				print "\t\treturn true;"
				print ""; print "\treturn false;"; print "}"
				skip = 1; next
			}
			skip && /^}$/ { skip = 0; next }
			!skip
			' "$f" > "$f.resolved" && mv "$f.resolved" "$f"
		fi
		;;
	*)
		echo "!! unhandled conflict: $f"
		rc=1
		continue
		;;
	esac
	git add "$f"
done

# the manager's versionCode formula is the source of truth for KSU_VERSION
if [ "$rc" = 0 ]; then
	# " - N" if the manager subtracts an offset, empty if it does not
	off="$(sed -n 's/.*return 30000 + commitCount\( - [0-9][0-9]*\)* *$/\1/p' manager/build.gradle.kts | head -1)"
	want="expr 30000 + \$(KSU_GIT_VERSION)$off"
	if grep -q 'KSU_GIT_VERSION)' kernel/Makefile && ! grep -qF "$want)" kernel/Makefile; then
		sed -i "s|expr 30000 + \$(KSU_GIT_VERSION)\( - [0-9]*\)\{0,1\}|$want|" kernel/Makefile
		echo "-- KSU_VERSION formula synced to manager (30000 + count$off)"
		if [ -d "$(git rev-parse --git-path rebase-merge)" ] ||
		   [ -d "$(git rev-parse --git-path rebase-apply)" ]; then
			git add kernel/Makefile
		fi
	fi
fi

exit $rc
