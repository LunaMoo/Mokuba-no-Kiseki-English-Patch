#!/bin/sh
# Usage: ./make_ppsspp_edition.sh <game.iso>   (decrypted EBOOT dump must be next to the ISO)
D=$(dirname "$0")
exec "$D/mokuba_patcher_linux" "$1" "$D/mokuba_english_ppsspp.patch"
