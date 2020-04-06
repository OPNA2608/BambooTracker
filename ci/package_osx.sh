#!/usr/bin/env bash

set -x
set -e

cp -a "$(find ../ -name BambooTracker.app)" ../{img,demos,licenses,specs,skins,*.md} .
macdeployqt BambooTracker.app -verbose=2
mv ../BambooTracker/.qm/ BambooTracker.app/Contents/Resources/lang

exit 0
