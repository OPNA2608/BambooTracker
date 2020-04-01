#!/usr/bin/env bash

set -e

if [[ "$#" -lt 1 ]]; then
  echo "Incomplete list of arguments." >&2
  echo "Need: <target-version>" >&2
  exit 1
fi

MINGW_TARGETVER="$1"

echo "Uninstalling existing MinGW."
choco uninstall -y mingw

echo "Installing requested x86 version of MinGW."
choco install -q mingw --version="$MINGW_TARGETVER" -x86 -params "/exception:dwarf"

echo "x86 MinGW installed."
exit 0
