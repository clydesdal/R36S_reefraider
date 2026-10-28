#!/bin/bash
# ArkOS Ports launcher. Copy to EASYROMS/ports/ with the reef/ folder beside it.
GAMEDIR=/roms/ports/reef
cd "$GAMEDIR" || exit 1
chmod +x ./reef
./reef > "$GAMEDIR/log.txt" 2>&1
