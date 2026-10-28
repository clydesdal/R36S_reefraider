#!/bin/bash
# Laptop build of Reef Raider: a 960x720 window, keyboard controls. ./host.sh && ./reef_host
set -e
cd "$(dirname "$0")"
gcc -O2 -g -Wall -Wextra -Wno-unused-parameter -DHOST -o reef_host reef.c wifi_font.c $(pkg-config --cflags --libs sdl2) -lm
echo "built $(pwd)/reef_host"
