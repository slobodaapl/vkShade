#!/bin/sh
# vkshade — launch a game with vkShade overlay enabled.
# Sets ENABLE_VKSHADE=1 and LD_AUDIT for Wine Wayland input interposition.

if [ "${1:-}" = "-v" ]; then
  export VKSHADE_LOG_LEVEL=debug
  shift
fi

case "${1:-}" in
--)
  shift
  ;;
-h | --help)
  echo "Usage: vkshade [-v] [--] <command...>"
  echo ""
  echo "Launch a game with vkShade overlay enabled."
  echo "Sets ENABLE_VKSHADE=1, LD_PRELOAD and LD_AUDIT for Wayland input interposition."
  exit 0
  ;;
esac

if [ $# -eq 0 ]; then
  echo "Usage: vkshade [-v] [--] <command...>"
  exit 0
fi

vkshade_wrapped_command=$1
case "$vkshade_wrapped_command" in
*/*) ;;
*)
  if vkshade_resolved_command=$(command -v "$vkshade_wrapped_command"); then
    vkshade_wrapped_command=$vkshade_resolved_command
  fi
  ;;
esac
case "$vkshade_wrapped_command" in
/*) ;;
*) vkshade_wrapped_command=$PWD/$vkshade_wrapped_command ;;
esac
export VKSHADE_WRAPPED_COMMAND="${VKSHADE_WRAPPED_COMMAND:-$vkshade_wrapped_command}"
export VKSHADE_WRAPPED_CWD="${VKSHADE_WRAPPED_CWD:-$PWD}"

export ENABLE_VKSHADE=1

vkshade_lib="@out@/lib/libvkshade.so"
case ":${LD_PRELOAD:-}:" in
*":$vkshade_lib:"*) ;;
*) export LD_PRELOAD="$vkshade_lib${LD_PRELOAD:+:$LD_PRELOAD}" ;;
esac

# LD_AUDIT for Wine Wayland: Wine loads winewayland.so via dlopen(RTLD_LOCAL),
# bypassing our wl_proxy_add_listener interposition. The audit library
# redirects symbol bindings in RTLD_LOCAL scopes to our wrapper.
audit_lib="@out@/lib/libvkshade-audit.so"
if [ -f "$audit_lib" ]; then
  export LD_AUDIT="${LD_AUDIT:+$LD_AUDIT:}$audit_lib"
fi

exec "$@"
