#!/usr/bin/env bash
# Build a release package: binary + stdlib + docs.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-release}"
DIST_DIR="${DIST_DIR:-$ROOT_DIR/dist}"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DA_WARNINGS_AS_ERRORS=ON \
  -DA_ENABLE_LTO=ON
cmake --build "$BUILD_DIR" -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
ctest --test-dir "$BUILD_DIR" --output-on-failure

version="$(sed -n 's/^project(Alang VERSION \([^ ]*\).*/\1/p' "$ROOT_DIR/CMakeLists.txt")"
platform="$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)"
name="a-${version}-${platform}"

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/$name/bin"
cp "$BUILD_DIR/a" "$stage/$name/bin/a"
cp -R "$ROOT_DIR/stdlib" "$stage/$name/stdlib"
cp "$ROOT_DIR/README.md" "$stage/$name/README.md"
tar -C "$stage" -czf "$DIST_DIR/$name.tar.gz" "$name" 2>/dev/null || {
  mkdir -p "$DIST_DIR"
  tar -C "$stage" -czf "$DIST_DIR/$name.tar.gz" "$name"
}
echo "wrote $DIST_DIR/$name.tar.gz"
