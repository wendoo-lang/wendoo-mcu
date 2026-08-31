#!/usr/bin/env bash
# C++ host check suite: configure + build + run tests for every preset
# (debug, release, sanitize), then enforce formatting. Run from anywhere;
# operates on the cpp/ tree it lives in, reconfiguring any preset whose cache
# was left behind by a build of a different source tree.
set -euo pipefail
cd "$(dirname "$0")"

source_dir="$(pwd -P)"

for preset in debug release sanitize; do
  echo "==> preset: ${preset}"
  # A cache recorded against a different source directory cannot be reused;
  # reconfigure that preset from scratch. Matching caches are left alone.
  cache="build/${preset}/CMakeCache.txt"
  if [ -f "${cache}" ]; then
    cached_source_dir="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${cache}")"
    if [ "${cached_source_dir}" != "${source_dir}" ]; then
      echo "stale cache: ${preset} was configured for '${cached_source_dir}'; reconfiguring"
      cmake --preset "${preset}" --fresh
    fi
  fi
  cmake --preset "${preset}"
  cmake --build --preset "${preset}"
  ctest --preset "${preset}"
done

clang_format=""
if command -v clang-format >/dev/null 2>&1; then
  clang_format="clang-format"
elif xcrun --find clang-format >/dev/null 2>&1; then
  clang_format="$(xcrun --find clang-format)"
else
  echo "error: clang-format not found (install LLVM or Xcode command line tools)" >&2
  exit 1
fi

echo "==> clang-format"
find core hostkit codal test tools targets/microbit-v2/abi targets/microbit-v2/source \
  -path '*/vendor' -prune -o \
  \( -name '*.h' -o -name '*.cpp' \) -print0 |
  xargs -0 "${clang_format}" --dry-run -Werror

echo "==> dependency guardrails"
./check-deps.sh

echo "cpp check: all checks passed"
