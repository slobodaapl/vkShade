#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo_root"
git rev-parse --show-toplevel >/dev/null
source scripts/files.sh

failed=0
build_dir=${VKSHADE_CHECK_BUILD_DIR:-build}
run_check() {
  local label=$1
  shift
  printf '\n==> %s\n' "$label"
  if ! command -v "$1" >/dev/null; then
    printf 'Missing required tool: %s\n' "$1" >&2
    failed=1
  elif ! "$@"; then
    printf 'Failed: %s\n' "$label" >&2
    failed=1
  fi
}

for file in "${nix_files[@]}"; do
  run_check "Nix formatting: $file" nixfmt --check "$file"
done
run_check 'Meson formatting' meson format --check-only "${meson_files[@]}"
run_check 'Justfile formatting' just --fmt --check
run_check 'C and C++ formatting' clang-format --dry-run --Werror --ferror-limit=1 "${native_files[@]}"
run_check 'Shell formatting' uvx --from shfmt-py==4.2.0 shfmt -d "${shell_files[@]}"
run_check 'Shell lint' scripts/shellcheck.sh -x "${shell_files[@]}"
run_check 'GitHub Actions lint' actionlint -shellcheck scripts/shellcheck.sh
run_check 'JavaScript lint' npx --yes eslint@10.14.0 --config scripts/eslint.config.cjs "${javascript_files[@]}"
run_check 'Workflow JavaScript lint' node scripts/check-workflow-js.cjs "${workflow_files[@]}"
for file in "${json_files[@]}"; do
  run_check "JSON syntax: $file" jq empty "$file"
done

if [[ ! -f $build_dir/build.ninja ]]; then
  run_check 'Meson setup' meson setup "$build_dir"
fi
run_check 'Build and shader compilation' meson compile -C "$build_dir"
run_check 'Meson tests' meson test -C "$build_dir" --print-errorlogs
run_check 'Standalone shader tester type check' c++ -std=c++20 -Isrc -Isrc/reshade -fsyntax-only tools/test_shaders.cpp
# Cppcheck does not infer compiler or Vulkan calling-convention macros from system headers.
run_check 'C and C++ static analysis' uvx --from cppcheck-py==2.22.0 cppcheck --project="$build_dir/compile_commands.json" \
  -D__GNUC__=4 -DVKAPI_ATTR= -DVKAPI_CALL= --enable=warning,performance,portability --error-exitcode=1 \
  --suppress=missingIncludeSystem '--suppress=*:src/reshade/*' '--suppress=*:src/imgui/*' '--suppress=*:src/proton/vendor/minhook/*' "--suppress=*:$build_dir/*" \
  -i src/imgui -i src/reshade -i src/proton/vendor/minhook -i src/stb_image.c -i src/stb_image_resize.c
run_check 'Proton Windows C static analysis' uvx --from cppcheck-py==2.22.0 cppcheck --platform=win64 --std=c11 -Isrc -Isrc/proton \
  -DMINHOOK_DISABLE_INTRINSICS --enable=warning,performance,portability --error-exitcode=1 --suppress=missingIncludeSystem \
  '--suppress=*:src/proton/vendor/minhook/*' src/proton/injector.c src/proton/streamline_observer.c \
  tests/proton_streamline_fake.c tests/proton_streamline_runtime.c
run_check 'Standalone shader tester static analysis' uvx --from cppcheck-py==2.22.0 cppcheck --std=c++20 -Isrc -Isrc/reshade \
  --enable=warning,performance,portability --error-exitcode=1 --suppress=missingIncludeSystem \
  '--suppress=*:src/reshade/*' tools/test_shaders.cpp

exit "$failed"
