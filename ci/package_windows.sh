#!/usr/bin/env bash

set -x
set -e

cp -a "$(find ../ -name BambooTracker.exe)" ../{img,demos,licenses,specs,skins,*.md} .
windeployqt BambooTracker.exe -verbose=2
mv translations lang
mv ../BambooTracker/.qm/*.qm lang/

exit 0
