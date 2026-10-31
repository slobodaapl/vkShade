#!/usr/bin/env bash

native_files=()
shell_files=()
nix_files=()
meson_files=()
json_files=()
workflow_files=()
javascript_files=()

while IFS= read -r -d '' file; do
  [[ -f "$file" ]] || continue
  case "$file" in
  src/imgui/* | src/reshade/* | src/shader/*.h | src/stb_image* | src/AreaTex.h | src/SearchTex.h) ;;
  *.c | *.cc | *.cpp | *.cxx | *.h | *.hh | *.hpp | *.hxx) native_files+=("$file") ;;
  esac

  case "$file" in
  src/imgui/* | src/reshade/*) ;;
  *.js | *.cjs | *.mjs) javascript_files+=("$file") ;;
  esac

  case "$file" in
  *.sh | scripts/vkshade-run.in) shell_files+=("$file") ;;
  esac
  case "$file" in
  *.nix) nix_files+=("$file") ;;
  meson.build | */meson.build | meson_options.txt) meson_files+=("$file") ;;
  *.json | *.json.in | flake.lock) json_files+=("$file") ;;
  .github/workflows/*.yml | .github/workflows/*.yaml) workflow_files+=("$file") ;;
  esac
done < <(git ls-files --cached --others --exclude-standard -z)
