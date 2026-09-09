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
		# upstream hand-bumps -DKSU_VERSION=NNNNN; we derive it from the
		# commit count and keep that number only as the no-git fallback.
		v="$(upstream_side "$f" | sed -n 's/.*-DKSU_VERSION=\([0-9][0-9]*\).*/\1/p' | head -1)"
		take_ours "$f"
		if [ -n "$v" ]; then
			sed -i "s/^KSU_VERSION := [0-9][0-9]*\$/KSU_VERSION := $v/" "$f"
		fi
		;;
	manager/build.gradle.kts|userspace/ksud/build.rs)
		# upstream re-tunes the offset it subtracts from the commit count to
		# line up with official numbering; we use the plain count everywhere,
		# so take whatever else upstream changed there and drop the offset.
		take_upstream "$f"
		sed -i -e 's/\(return 30000 + commitCount\) - [0-9][0-9]*/\1/' \
		       -e 's/\(let version_code = 30000 + version_code\) - [0-9][0-9]*;/\1;/' "$f"
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
		# upstream keeps adding fallback signing keys; we accept only ours.
		take_ours "$f"
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
	if ! grep -qF "$want)" kernel/Makefile; then
		sed -i "s|expr 30000 + \$(KSU_GIT_VERSION)\( - [0-9]*\)\{0,1\}|$want|" kernel/Makefile
		echo "-- KSU_VERSION formula synced to manager (30000 + count$off)"
		if [ -d "$(git rev-parse --git-path rebase-merge)" ] ||
		   [ -d "$(git rev-parse --git-path rebase-apply)" ]; then
			git add kernel/Makefile
		fi
	fi
fi

exit $rc
