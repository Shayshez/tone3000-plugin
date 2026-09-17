#!/bin/bash
# Bump the sandbox/experiments checkpoint counter and log what shipped.
#
# Usage:
#   ./script/bump-sandbox-build.sh "short description of what's stable now"
#
# This is NOT the plugin version (see script/set-version.sh) - it's a
# dev-only marker for this branch, shown faintly in Settings under the real
# version ("sandbox build N"), so a running build can be pointed back at the
# SANDBOX_LOG.md entry that explains what's in it. Both files disappear
# (harmlessly - see the CMake/native guards) once this branch merges.
#
# What it does:
#   1. Reads SANDBOX_BUILD, increments it.
#   2. Appends a dated entry to SANDBOX_LOG.md with the new number, the
#      current git short hash, and your description.
# It does NOT commit - review and commit SANDBOX_BUILD + SANDBOX_LOG.md
# yourself alongside the actual work that made this a stable point.

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 \"description of what's stable now\"" >&2
  exit 1
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DESC="$1"

CURRENT="$(cat "$ROOT/SANDBOX_BUILD")"
NEXT=$((CURRENT + 1))
echo "$NEXT" > "$ROOT/SANDBOX_BUILD"

HASH="$(git -C "$ROOT" rev-parse --short HEAD)"
DATE="$(date +%Y-%m-%d)"

{
  echo ""
  echo "### Build $NEXT — $DATE — $HASH"
  echo ""
  echo "$DESC"
} >> "$ROOT/SANDBOX_LOG.md"

echo "Bumped sandbox build $CURRENT -> $NEXT, logged to SANDBOX_LOG.md:"
echo "  ### Build $NEXT — $DATE — $HASH"
echo "  $DESC"
echo ""
echo "Review SANDBOX_LOG.md, then commit both files with the work itself."
