#!/bin/sh
#
# Build notebooks-<version>.zip: unzip it into the root of the Kindle.
# Expects both helper builds in dist/ (from CI, or copied there by hand).
#
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
VERSION="${VERSION:-0.0.1}"
STAGING="$ROOT/build/package"
OUT="$ROOT/release"
APP="$STAGING/extensions/notebooks"

rm -rf "$STAGING"
mkdir -p "$APP/bin" "$APP/notebooks-app" "$STAGING/documents" "$OUT"

for NAME in notebooks_helper notebooks_helper_hf; do
    cp "$ROOT/dist/$NAME" "$APP/bin/$NAME"
    chmod +x "$APP/bin/$NAME"
done

cp "$ROOT"/app/index.html "$ROOT"/app/app.css "$ROOT"/app/app.js "$ROOT"/app/lib.js "$ROOT"/app/config.xml "$APP/notebooks-app/"
cp "$ROOT/src/scripts/launch.sh" "$APP/launch.sh"
cp "$ROOT/packaging/icon.png" "$APP/icon.png"
cp "$ROOT/LICENSE" "$APP/LICENSE"
cp "$ROOT/packaging/documents/Notebooks.sh" "$STAGING/documents/Notebooks.sh"
echo "$VERSION" > "$APP/VERSION"

rm -f "$OUT/notebooks-$VERSION.zip"
(cd "$STAGING" && zip -r "$OUT/notebooks-$VERSION.zip" documents extensions)
echo "Created $OUT/notebooks-$VERSION.zip"
