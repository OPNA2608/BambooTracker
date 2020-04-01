#!/usr/bin/env bash

set -e

PKGDIR=BambooTracker-"$TRAVIS_TAG"
mkdir "$PKGDIR"
cp -at "$PKGDIR" "$(find . -name BambooTracker.exe)" ../*.md ../img ../demos ../licenses ../specs ../skins
cd "$PKGDIR"
windeployqt BambooTracker.exe -verbose=2
mv translations lang
mv ../.qm/*.qm lang/
7z a -tzip ../../"$PKGDIR"-"$TARGET_OS".zip *

exit 0
