#!/usr/bin/env bash
# Apply the GCC/mcpp source-conformance fixes to the submodules.
#
# These fixes live in OTHER repositories (Yuria-Shikibe/mo_yanxi_utility and
# .../mo_yanxi_vulkan_wrapper), so a commit in xrgui cannot carry them. Until
# they are upstreamed they are kept here as patches and replayed on a fresh
# checkout.
#
#   git submodule update --init --recursive
#   ./mcpp/patches/apply.sh
#   mcpp build
#
# Every hunk is a portability fix that MSVC accepts identically -- none of them
# is an mcpp-only workaround, so upstreaming them is pure gain for the xmake
# build too. See .agents/docs/2026-08-12-mcpp-adaptation-design.md §11.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

apply_one() {
    local repo="$1" patch="$2"
    if [[ ! -d "$root/$repo/.git" && ! -f "$root/$repo/.git" ]]; then
        echo "error: $repo is not checked out -- run: git submodule update --init --recursive" >&2
        exit 1
    fi
    if git -C "$root/$repo" apply --check --reverse "$root/$patch" 2>/dev/null; then
        echo "already applied: $repo"
        return
    fi
    git -C "$root/$repo" apply "$root/$patch"
    echo "applied: $repo"
}

apply_one external/mo_yanxi_vulkan_wrapper \
          mcpp/patches/mo_yanxi_vulkan_wrapper.patch
apply_one external/mo_yanxi_vulkan_wrapper/external/mo_yanxi_utility \
          mcpp/patches/mo_yanxi_utility.patch
apply_one external/allocator2d \
          mcpp/patches/allocator2d.patch
