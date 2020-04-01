#!/usr/bin/env bash

set -e

PKGDIR=BambooTracker-"$TRAVIS_TAG"
mkdir "$PKGDIR"
cp -a "$(find . -name BambooTracker.app)" ../*.md ../img ../demos ../licenses ../specs ../skins "$PKGDIR"
cd "$PKGDIR"
macdeployqt BambooTracker.app -verbose=2
mv ../.qm/ BambooTracker.app/Contents/Resources/lang
7z a -tzip ../../"$PKGDIR"-"$TARGET_OS".zip *

exit 0
