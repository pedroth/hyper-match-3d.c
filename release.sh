#!/bin/sh
set -eu

# Edit this value before running the script.
VERSION="v0.0.2"

case "$VERSION" in
  v[0-9]*.[0-9]*.[0-9]*) ;;
  *)
    echo "Version must match the format vX.Y.Z"
    exit 1
    ;;
esac

if git rev-parse -q --verify "refs/tags/$VERSION" >/dev/null 2>&1; then
  echo "Tag $VERSION already exists. Nothing to do."
  exit 0
fi

CURRENT_BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || printf '')
if [ "$CURRENT_BRANCH" != "main" ]; then
  echo "You are on '$CURRENT_BRANCH', not 'main'."
  echo "Checkout main before creating a release tag."
  exit 1
fi

git pull --ff-only origin main

git tag "$VERSION"
git push origin "$VERSION"

echo "Created and pushed tag: $VERSION"
