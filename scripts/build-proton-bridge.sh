#!/bin/sh
set -eu

mode=$1
toolchain=$2
compiler=$3
linker=$4
src=$5
build=$6
tmp=$(mktemp -d "$build/.proton-bridge.XXXXXXXX")
trap 'rm -rf "$tmp"' EXIT
trap 'exit 1' HUP INT TERM

if [ "$mode" = probe ]; then
  injector=$tmp/vkshade-proton-injector.exe
  observer=$tmp/vkshade-proton-observer.dll
else
  injector=$7
  observer=$8
fi

proton=$src/proton
minhook=$proton/vendor/minhook/src
if [ "$toolchain" = clang ]; then
  compile() {
    "$compiler" --target=x86_64-pc-windows-msvc -std=c11 -O2 \
      -fno-stack-protector -DMINHOOK_DISABLE_INTRINSICS \
      -I/usr/include/wine/msvcrt -I/usr/include/wine/windows \
      -I/usr/include/wine -I"$src" -c "$1" -o "$2"
  }
else
  compile() {
    "$compiler" -std=c11 -O2 -DMINHOOK_DISABLE_INTRINSICS -I"$src" -c "$1" -o "$2"
  }
fi

if [ "$mode" = fixture ]; then
  compile "$src/../tests/proton_streamline_fake.c" "$tmp/fake.o"
  compile "$src/../tests/proton_streamline_runtime.c" "$tmp/runtime.o"
  if [ "$toolchain" = clang ]; then
    libs=/usr/lib/wine/x86_64-windows
    "$linker" /dll /noentry /out:"$injector" /implib:"$tmp/fake.lib" \
      "$tmp/fake.o" "$libs/libkernel32.a" "$libs/libshell32.a" "$libs/libucrtbase.a"
    "$linker" /subsystem:console /entry:mainCRTStartup /out:"$observer" \
      "$tmp/runtime.o" "$libs/libkernel32.a" "$libs/libshell32.a" "$libs/libucrtbase.a"
  else
    "$compiler" -shared -nostartfiles -Wl,--entry,0 -o "$injector" \
      "$tmp/fake.o" -lshell32 -lkernel32
    "$compiler" -nostartfiles -Wl,-e,mainCRTStartup -o "$observer" \
      "$tmp/runtime.o" -lshell32 -lkernel32
  fi
  exit 0
fi

if [ "$mode" = probe ]; then
  printf '%s\n' \
    '#include <windows.h>' \
    '#include <shellapi.h>' \
    '#include <tlhelp32.h>' \
    '#include <stdint.h>' \
    '#include <stdio.h>' \
    '#include <stdlib.h>' \
    '#include <string.h>' \
    '#include <wchar.h>' \
    'void mainCRTStartup(void) {' \
    '    int argc; WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);' \
    '    fwprintf(stderr, L"%d", argc); LocalFree(argv); ExitProcess(0);' \
    '}' >"$tmp/probe.c"
  compile "$tmp/probe.c" "$tmp/injector.o"
  set -- "$tmp/injector.o"
else
  compile "$proton/injector.c" "$tmp/injector.o"
  compile "$proton/streamline_observer.c" "$tmp/observer.o"
  compile "$minhook/buffer.c" "$tmp/buffer.o"
  compile "$minhook/hook.c" "$tmp/hook.o"
  compile "$minhook/trampoline.c" "$tmp/trampoline.o"
  compile "$minhook/hde/hde64.c" "$tmp/hde64.o"
  set -- "$tmp/observer.o" "$tmp/buffer.o" "$tmp/hook.o" "$tmp/trampoline.o" "$tmp/hde64.o"
fi

if [ "$toolchain" = clang ]; then
  libs=/usr/lib/wine/x86_64-windows
  "$linker" /subsystem:console /entry:mainCRTStartup /out:"$injector" \
    "$tmp/injector.o" "$libs/libkernel32.a" "$libs/libshell32.a" "$libs/libucrtbase.a"
  "$linker" /dll /noentry /out:"$observer" /implib:"$tmp/observer.lib" \
    "$@" "$libs/libkernel32.a" "$libs/libshell32.a" "$libs/libucrtbase.a"
else
  "$compiler" -nostartfiles -Wl,-e,mainCRTStartup -o "$injector" \
    "$tmp/injector.o" -lshell32 -lkernel32
  "$compiler" -shared -nostartfiles -Wl,--entry,0 -o "$observer" \
    "$@" -lshell32 -lkernel32
fi
