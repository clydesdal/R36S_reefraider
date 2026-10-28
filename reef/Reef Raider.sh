#!/bin/bash
# Ports launcher for Reef Raider. Put this script in your ports folder with the reef/ folder beside it.
GAMEDIR="$(cd "$(dirname "$0")" && pwd)/reef"
[ -x "$GAMEDIR/reef" ] || [ -f "$GAMEDIR/reef" ] || GAMEDIR=/roms/ports/reef
cd "$GAMEDIR" || exit 1
chmod +x ./reef
./reef > "$GAMEDIR/log.txt" 2>&1
