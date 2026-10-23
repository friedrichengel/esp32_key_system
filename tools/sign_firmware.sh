#!/usr/bin/env sh
# Signiert ein Firmware-Image manuell (normalerweise erledigt das tools/sign_firmware.py
# automatisch beim PlatformIO-Build) und prüft die Signatur.
#
#   tools/sign_firmware.sh build/esp32_key_system.bin [keys/ota_signing_key.pem] [out.bin]
#
# Das Eingabe-Image muss mit "elf2image --secure-pad-v2" erzeugt worden sein
# (Größe Vielfaches von 4096), sonst lehnt espsecure es ab.
set -eu
IN="${1:?Pfad zum unsignierten .bin angeben}"
KEY="${2:-keys/ota_signing_key.pem}"
OUT="${3:-${IN%.bin}-signed.bin}"

if command -v espsecure >/dev/null 2>&1; then
  ESPSECURE=espsecure
elif [ -x "$HOME/.platformio/packages/tool-esptool/espsecure.exe" ]; then
  ESPSECURE="$HOME/.platformio/packages/tool-esptool/espsecure.exe"
else
  ESPSECURE="$HOME/.platformio/packages/tool-esptool/espsecure"
fi

"$ESPSECURE" sign-data --version 2 --keyfile "$KEY" --output "$OUT" "$IN"
"$ESPSECURE" verify-signature --version 2 --keyfile "$KEY" "$OUT"
"$ESPSECURE" signature-info-v2 "$OUT"
echo "Signiertes Image: $OUT  (dieses über die Firmware-Seite hochladen)"
