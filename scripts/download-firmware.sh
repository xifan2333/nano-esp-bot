#!/usr/bin/env bash
set -euo pipefail

DEST_DIR="${FIRMWARE_DIR:-dist/firmware}"
CURRENT_BRANCH=$(git branch --show-current 2>/dev/null || echo "main")
ARTIFACT_NAME="nano-esp-bot-firmware"

echo "==> Resolving latest CI firmware artifact for branch: $CURRENT_BRANCH..."

# Try to find the latest successful CI run on the current branch
RUN_ID=$(gh run list --branch "$CURRENT_BRANCH" --workflow ci.yml --status success --limit 1 --json databaseId -q '.[0].databaseId' 2>/dev/null || true)

# If no successful run on current branch, fallback to main
if [ -z "$RUN_ID" ] || [ "$RUN_ID" = "null" ]; then
    echo "Notice: No successful CI run found on branch '$CURRENT_BRANCH'. Falling back to 'main'..."
    RUN_ID=$(gh run list --branch main --workflow ci.yml --status success --limit 1 --json databaseId -q '.[0].databaseId' 2>/dev/null || true)
fi

if [ -z "$RUN_ID" ] || [ "$RUN_ID" = "null" ]; then
    echo "Error: No successful CI run found with artifact '$ARTIFACT_NAME' on '$CURRENT_BRANCH' or 'main'." >&2
    exit 1
fi

echo "==> Downloading artifact from CI run #$RUN_ID into $DEST_DIR..."
mkdir -p "$DEST_DIR"
gh run download "$RUN_ID" -n "$ARTIFACT_NAME" -D "$DEST_DIR"

echo "==> Download complete! Firmware files in $DEST_DIR:"
ls -lh "$DEST_DIR"
