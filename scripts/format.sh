#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo_root"
git rev-parse --show-toplevel >/dev/null
source scripts/files.sh

for tool in nixfmt meson just clang-format uvx npx; do
  if ! command -v "$tool" >/dev/null; then
    printf 'Missing required tool: %s\n' "$tool" >&2
    exit 1
  fi
done

for file in "${nix_files[@]}"; do
  nixfmt "$file"
done
meson format --inplace "${meson_files[@]}"
just --fmt
clang-format -i "${native_files[@]}"
uvx --from shfmt-py==4.2.0 shfmt -w "${shell_files[@]}"
npx --yes eslint@10.14.0 --config scripts/eslint.config.cjs --fix "${javascript_files[@]}"
