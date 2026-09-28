#!/usr/bin/env bash
# Land docs/ edits on the private docs repo (opennova-net/docs) and stage the new
# pointer here, so the pointer bump rides the slice's own commit.
#
#   scripts/land_docs.sh "<commit message>"   commit docs/, rebase it onto docs
#                                             master, push, stage the pointer
#   scripts/land_docs.sh --sync               move the pointer to the docs master
#                                             tip (resolves pointer conflicts
#                                             between PRs: docs master holds both)
#
# docs/ is checked out only where `git config submodule.docs.update checkout` is
# set (CLAUDE.md). Docs master is never force-pushed: every pointer this repo
# ever recorded must stay reachable. On a rebase conflict, resolve it in docs/
# (`git -C docs rebase --continue`) and run the command again.
set -euo pipefail

root=$(git rev-parse --show-toplevel)
docs="$root/docs"
if [ ! -e "$docs/.git" ]; then
    echo "land_docs: docs/ is not checked out; run" \
         "git config submodule.docs.update checkout && git submodule update --init docs" >&2
    exit 1
fi

if [ "${1:-}" = "--sync" ] && [ $# -eq 1 ]; then
    if [ -n "$(git -C "$docs" status --porcelain)" ]; then
        echo "land_docs: docs/ has uncommitted edits; land them first" >&2
        exit 1
    fi
    git -C "$docs" fetch -q origin master
    git -C "$docs" checkout -q --detach origin/master
elif [ $# -eq 1 ] && [ -n "$1" ]; then
    git -C "$docs" add -A
    if ! git -C "$docs" diff --cached --quiet; then
        git -C "$docs" commit -q -m "$1"
    fi
    git -C "$docs" fetch -q origin master
    git -C "$docs" rebase -q origin/master
    git -C "$docs" push -q origin HEAD:master
else
    echo "usage: scripts/land_docs.sh \"<commit message>\" | --sync" >&2
    exit 2
fi

git -C "$root" add docs
echo "land_docs: docs/ at $(git -C "$docs" rev-parse --short HEAD) (docs master); pointer staged"
