#!/bin/sh
# Name: Notebooks
# Icon: /mnt/us/extensions/notebooks/icon.png
# DontUseFBInk

APP="/mnt/us/extensions/notebooks"

if [ ! -f "$APP/launch.sh" ]; then
    exit 1
fi

exec sh "$APP/launch.sh"
