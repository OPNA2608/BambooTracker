#!/usr/bin/env bash

echo "OS: ""$TRAVIS_OS_NAME"
if [ "$TRAVIS_OS_NAME" == "osx" ]; then

  set -o xtrace
  # macOS: install Qt5 per brew
  brew install qt5
  brew link --force qt5
  
elif [ "$TRAVIS_OS_NAME" == "linux" ]; then

  set -o xtrace
  # Linux: add PPA, download Qt5 per apt
  sudo add-apt-repository --yes ppa:ubuntu-sdk-team/ppa
  sudo apt update -qq
  sudo apt install qt5-default qttools5-dev-tools qtmultimedia5-dev libqt5multimedia5-plugins
  sudo apt install libasound2-dev
  
else
  
  # Not-yet-added OS, can't know how to get Qt5 setup
  echo "Unknown OS name '""$TRAVIS_OS_NAME""', don't know how to acquire Qt5!" 1>&2
  echo "Please add instructions to '.travis/beforeInstall.sh'!" 1>&2
  exit 1
  
fi

