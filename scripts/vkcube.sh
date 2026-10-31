#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$repo_root/build"
status_file=$(mktemp "$repo_root/build/.vkcube-status.XXXXXX")
gamescope_pid=
gamescope_running() {
  [[ -n $gamescope_pid ]] && jobs -pr | grep -qx -- "$gamescope_pid"
}
# Invoked by the EXIT trap.
# shellcheck disable=SC2329
cleanup() {
  if gamescope_running; then
    kill -TERM -- "-$gamescope_pid" 2>/dev/null || true
    for _ in {1..20}; do
      gamescope_running || break
      sleep 0.1
    done
    if gamescope_running; then
      kill -KILL -- "-$gamescope_pid" 2>/dev/null || true
    fi
  fi
  if [[ -n $gamescope_pid ]]; then
    wait "$gamescope_pid" 2>/dev/null || true
  fi
  rm -f "$status_file"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

cube_env=()
while (($#)) && [[ $1 =~ ^[a-zA-Z_][a-zA-Z0-9_]*= ]]; do
  cube_env+=("$1")
  shift
done
if [[ ${1:-} == -- ]]; then
  shift
fi
cube_args=("$@")
has_frame_count=false
for arg in "${cube_args[@]}"; do
  if [[ $arg == --c ]]; then
    has_frame_count=true
    break
  fi
done
if ! $has_frame_count; then
  cube_args=(--c 120 "${cube_args[@]}")
fi

# shellcheck disable=SC2016
setsid gamescope --backend headless --expose-wayland -w 500 -h 500 -- \
  env "${cube_env[@]}" DISABLE_GAMESCOPE_WSI=1 sh -c '
    status_file=$1
    shift
    vkcube --wsi wayland "$@"
    status=$?
    printf "%s\n" "$status" > "$status_file"
    exit "$status"
  ' vkcube "$status_file" "${cube_args[@]}" &
gamescope_pid=$!

while [[ ! -s $status_file ]] && gamescope_running; do
  sleep 0.1
done

if [[ -s $status_file ]]; then
  read -r cube_status <"$status_file"
  exit "$cube_status"
fi
gamescope_status=0
wait "$gamescope_pid" || gamescope_status=$?
gamescope_pid=
((gamescope_status != 0)) && exit "$gamescope_status"
exit 1
