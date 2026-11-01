#!/bin/sh
set -eu

wine=$1
winepath=$2
wineserver=$3
fake=$4
driver=$5
observer=$6
injector=$7
build=$8

tmp=$(mktemp -d "$build/.proton-streamline-test.XXXXXXXX")
export WINEPREFIX="$tmp/prefix"
cleanup() {
  "$wineserver" -k || :
  "$wineserver" -w || :
  rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

cp "$fake" "$tmp/sl.interposer.dll"
cp "$driver" "$tmp/driver.exe"
cp "$observer" "$tmp/vkshade_streamline_observer.dll"
cp "$injector" "$tmp/injector.exe"
injector_dos=$("$winepath" -w "$tmp/injector.exe")
injector_dos=$(printf '%s' "$injector_dos" | tr -d '\r')

"$wine" "$tmp/driver.exe" --preloaded
"$wine" "$tmp/driver.exe" --late-load
"$wine" "$tmp/driver.exe" --metadata-stress
"$wine" "$tmp/driver.exe" --injector-contract "$injector_dos" "$tmp/vkshade_streamline_observer.dll"
