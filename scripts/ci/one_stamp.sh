#!/usr/bin/env bash
# The ONE stamp a CI build carries (engine/base/version/one_build.h): the values of the
# OPENNOVA_VERSION, OPENNOVA_STAGING, OPENNOVA_COMMIT and OPENNOVA_BUILD_TIME cache variables
# (engine/base/CMakeLists.txt), as key=value lines for a step's $GITHUB_OUTPUT:
#
#   scripts/ci/one_stamp.sh <commit> [<run number>]
#
#   version  the latest v* tag reachable from <commit> without its v (v0.0.22: 0.0.22), then
#            ".<run number>" when one is given (0.0.22.123)
#   staging  ON with a run number (a pull request's build, which staging publishes), else OFF
#   commit   <commit>'s sha, its first seven
#   time     <commit>'s commit time, Unix seconds
#
# ci.yml stamps every build of a run from its `changes` job's one call (a pull request's head
# and the run's number; a master push's commit alone), and publish.yml's staging release asks
# the same of the run's head and number, so the version Publish gives a release is the one its
# build carries. Run inside a checkout of the repository. A shallow one (CI's) holds no history
# to describe, so the commits and the tags alone (no trees, no blobs, a few MB) are fetched from
# its origin into a scratch repository: deepening the checkout itself turns it into a partial
# clone whose connectivity check then fetches the history's trees one at a time.
set -euo pipefail

usage="usage: scripts/ci/one_stamp.sh <commit> [<run number>]"
[[ $# -ge 1 && $# -le 2 ]] || { echo "$usage" >&2; exit 2; }
commit="$1"
run="${2:-}"
[[ -z "$run" || "$run" =~ ^[0-9]+$ ]] || { echo "one_stamp.sh: the run number is digits, not '$run'" >&2; exit 2; }

sha="$(git rev-parse --verify --quiet "$commit^{commit}" || true)"
history=()
if [[ "$(git rev-parse --is-shallow-repository)" == true || -z "$sha" ]]; then
    # A full sha (a pull request's head, which the merge commit a pull request's run checks out
    # names as its parent; GitHub serves any commit by its sha).
    sha="${sha:-$commit}"
    [[ "$sha" =~ ^[0-9a-f]{40}$ ]] || { echo "one_stamp.sh: '$commit' is no commit here; name it by its full sha" >&2; exit 2; }
    scratch="$(mktemp -d)"
    trap 'rm -rf "$scratch"' EXIT
    git init --quiet --bare "$scratch"
    git -C "$scratch" fetch --quiet --filter=tree:0 --tags "$(git remote get-url origin)" "$sha"
    history=(-C "$scratch")
fi

tag="$(git "${history[@]}" describe --tags --abbrev=0 --match 'v[0-9]*' "$sha")"
version="${tag#v}"
# Three numbers at most, so a run's number makes the fourth (engine/base/CMakeLists.txt checks
# the rest).
[[ "$version" =~ ^[0-9]+(\.[0-9]+){0,2}$ ]] || { echo "one_stamp.sh: $tag is not a release tag (v0.0.22)" >&2; exit 1; }
staging=OFF
if [[ -n "$run" ]]; then
    version="$version.$run"
    staging=ON
fi

echo "version=$version"
echo "staging=$staging"
echo "commit=${sha:0:7}"
echo "time=$(git "${history[@]}" log -1 --format=%ct "$sha")"
