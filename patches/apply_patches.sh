#!/bin/bash
# ============================================================================
# apply_patches.sh — apply the patches this device tree carries against its
# upstream repos (e.g. bootable/recovery). Run from anywhere after syncing the
# manifest and before building:
#
#     ./patches/apply_patches.sh
#
# Layout: patches/<repo/path>/NNNN-*.patch  ->  applied to  <repo/path>
# Idempotent: patches that are already applied are skipped.
# ============================================================================
set -e
TOP="$(cd "$(dirname "$0")/.." && pwd)"   # build top (parent of patches/)
cd "$TOP"

found=0
while IFS= read -r p; do
    found=1
    rel="${p#patches/}"          # bootable/recovery/0001-....patch
    repo="$(dirname "$rel")"     # bootable/recovery
    abs="$TOP/$p"
    if [ ! -d "$repo" ]; then
        echo "SKIP   $rel  (target '$repo' not present)"
        continue
    fi
    if git -C "$repo" apply --reverse --check "$abs" 2>/dev/null; then
        echo "OK     $rel  (already applied)"
    elif git -C "$repo" apply --check "$abs" 2>/dev/null; then
        git -C "$repo" apply "$abs"
        echo "APPLY  $rel"
    else
        echo "FAIL   $rel  (does not apply cleanly to '$repo' — check manually)" >&2
    fi
done < <(find patches -type f -name '*.patch' | sort)

[ "$found" = 1 ] || echo "no patches found under patches/"
