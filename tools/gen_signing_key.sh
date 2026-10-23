#!/usr/bin/env sh
# Erzeugt einmalig den privaten RSA-3072-Schlüssel zum Signieren der Firmware.
# Der Schlüssel bleibt auf dem Build-Rechner (keys/ ist in .gitignore).
# espsecure wird im PATH oder im PlatformIO-Paket tool-esptool gesucht.
set -eu
KEY="${1:-keys/ota_signing_key.pem}"
PUB="${KEY%.pem}_pub.pem"

if command -v espsecure >/dev/null 2>&1; then
  ESPSECURE=espsecure
elif [ -x "$HOME/.platformio/packages/tool-esptool/espsecure.exe" ]; then
  ESPSECURE="$HOME/.platformio/packages/tool-esptool/espsecure.exe"
elif [ -x "$HOME/.platformio/packages/tool-esptool/espsecure" ]; then
  ESPSECURE="$HOME/.platformio/packages/tool-esptool/espsecure"
else
  echo "espsecure nicht gefunden (ESP-IDF export.sh ausführen oder PlatformIO-Build einmal laufen lassen)." >&2
  exit 1
fi

if [ -f "$KEY" ]; then
  echo "Schlüssel existiert bereits: $KEY – nicht überschrieben." >&2
  exit 1
fi
mkdir -p "$(dirname "$KEY")"
"$ESPSECURE" generate-signing-key --version 2 --scheme rsa3072 "$KEY"
"$ESPSECURE" extract-public-key --version 2 --keyfile "$KEY" "$PUB"
echo "Signaturschlüssel erzeugt: $KEY  (öffentlich: $PUB)"
echo "WICHTIG: Schlüssel sichern. Geräte akzeptieren später nur Images, die mit diesem Schlüssel signiert sind."
