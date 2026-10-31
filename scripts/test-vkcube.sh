#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$repo_root/build"
test_dir=$(mktemp -d "$repo_root/build/.vkcube-test.XXXXXX")
pid_file="$test_dir/pid"

# Invoked by the EXIT trap.
# shellcheck disable=SC2329
cleanup() {
  if [[ -f $pid_file ]]; then
    read -r pid <"$pid_file"
    if [[ -r /proc/$pid/cmdline ]] && tr '\0' '\n' <"/proc/$pid/cmdline" | grep -Fxq "$test_dir/gamescope"; then
      kill -KILL -- "-$pid" 2>/dev/null || true
    fi
  fi
  rm -rf "$test_dir"
}
trap cleanup EXIT

printf '#!%s\n' "$(command -v sh)" >"$test_dir/gamescope"
cat >>"$test_dir/gamescope" <<'EOF'
while [ "$#" -gt 0 ]; do
  if [ "$1" = -- ]; then shift; break; fi
  shift
done
printf '%s\n' "$$" > "$VKCUBE_TEST_PID"
printf '%s\n' "${VKCUBE_TEST_VALUE-unset}" > "$VKCUBE_TEST_PARENT_ENV"
"$@"
if [ "${VKCUBE_TEST_HANG:-0}" = 1 ]; then
  trap '' TERM
  while :; do sleep 1; done
fi
exit 0
EOF
printf '#!%s\n' "$(command -v sh)" >"$test_dir/vkcube"
cat >>"$test_dir/vkcube" <<'EOF'
printf '%s\n' "$@" > "$VKCUBE_TEST_ARGS"
printf '%s\n' "${VKCUBE_TEST_VALUE-unset}" > "$VKCUBE_TEST_CHILD_ENV"
exit "${VKCUBE_TEST_EXIT:-0}"
EOF
chmod +x "$test_dir/gamescope" "$test_dir/vkcube"

export PATH="$test_dir:$PATH"
export VKCUBE_TEST_PID="$pid_file"
export VKCUBE_TEST_ARGS="$test_dir/args"
export VKCUBE_TEST_PARENT_ENV="$test_dir/parent-env"
export VKCUBE_TEST_CHILD_ENV="$test_dir/child-env"

status=0
timeout -k 3 8 "$repo_root/scripts/vkcube.sh" VKCUBE_TEST_EXIT=7 VKCUBE_TEST_VALUE=cube -- --c 3 || status=$?
[[ $status == 7 ]]
printf '%s\n' --wsi wayland --c 3 | cmp - "$VKCUBE_TEST_ARGS"
[[ $(<"$VKCUBE_TEST_PARENT_ENV") == unset ]]
[[ $(<"$VKCUBE_TEST_CHILD_ENV") == cube ]]

VKCUBE_TEST_HANG=1 timeout -k 3 8 "$repo_root/scripts/vkcube.sh" -- --validate
printf '%s\n' --wsi wayland --c 120 --validate | cmp - "$VKCUBE_TEST_ARGS"
read -r pid <"$pid_file"
! kill -0 "$pid" 2>/dev/null
