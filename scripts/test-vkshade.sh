#!/usr/bin/env bash
set -euo pipefail

launcher=$1
nix_template=$2
build_dir=$3
library=$4
audit_library=$5
scratch=$(mktemp -d "$build_dir/vkshade-launcher-test.XXXXXX")
trap 'rm -rf "$scratch"' EXIT

mkdir -p "$scratch/lib"
ln -s "$library" "$scratch/lib/libvkshade.so"
ln -s "$audit_library" "$scratch/lib/libvkshade-audit.so"
sed "s|@out@|$scratch|g" "$nix_template" >"$scratch/vkshade"
chmod +x "$scratch/vkshade"

for command in "$launcher" "$scratch/vkshade"; do
  # shellcheck disable=SC2016
  env -u VKSHADE_LOG_LEVEL -u ENABLE_VKSHADE -u LD_PRELOAD -u LD_AUDIT -u VKSHADE_WRAPPED_COMMAND -u VKSHADE_WRAPPED_CWD "$command" -- sh -c '
    test -z "${VKSHADE_LOG_LEVEL+x}"
    test "$ENABLE_VKSHADE" = 1
    test "$VKSHADE_WRAPPED_COMMAND" = "$(command -v sh)"
    test "$VKSHADE_WRAPPED_CWD" = "$PWD"
    test "$1" = --game-option
  ' test --game-option
  # shellcheck disable=SC2016
  env -u ENABLE_VKSHADE -u LD_PRELOAD -u LD_AUDIT VKSHADE_LOG_LEVEL=error "$command" -v -- sh -c '
    test "$VKSHADE_LOG_LEVEL" = debug
    test "$ENABLE_VKSHADE" = 1
    test -r "$LD_PRELOAD"
    test "$1" = --game-option
  ' test --game-option
  if "$command" -- sh -c 'exit 37'; then
    exit 1
  else
    test "$?" = 37
  fi
done

if "$launcher" -v -- >/dev/null 2>&1; then
  exit 1
else
  test "$?" = 2
fi
