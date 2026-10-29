"""
PlatformIO extra script (post:) – signiert firmware.bin nach dem Build.

Hintergrund: Die Firmware läuft im ESP-IDF-Modus "Signed App Verification without
Hardware Secure Boot" (CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT). esp_ota_end() prüft
jedes OTA-Image gegen den öffentlichen Schlüssel im Signaturblock der *laufenden* App.
Der PlatformIO-Builder (framework espidf) hängt diesen Block aber nur bei echtem
Hardware-Secure-Boot (CONFIG_SECURE_BOOT) an. Dieses Skript holt das nach:

  1. elf2image bekommt --secure-pad-v2, damit das Image + 4 KB Signaturblock auf einer
     MMU-Seite (64 KB beim ESP32-S31) endet.
  2. Nach dem Erzeugen von firmware.bin wird es per espsecure (RSA-3072, Secure Boot v2
     Format) signiert und in-place ersetzt. Damit flasht "pio run -t upload" das signierte
     Image, und dieselbe Datei kann über die Firmware-Seite hochgeladen werden.

Schlüsselsuche (Reihenfolge):
  1. Umgebungsvariable OTA_SIGNING_KEY (Pfad zu einer PEM-Datei)
  2. CONFIG_SECURE_BOOT_SIGNING_KEY aus sdkconfig.defaults (Default: keys/ota_signing_key.pem)

Ohne Schlüssel bleibt firmware.bin unsigniert und es gibt eine deutliche Warnung. Ein
unsigniertes Image bootet per USB-Flash zwar, kann aber keine OTA-Updates annehmen.
"""

import os
import re
import shutil
import subprocess
import sys

Import("env")  # noqa: F821 - von PlatformIO/SCons injiziert

PROJECT_DIR = env["PROJECT_DIR"]  # noqa: F821
platform = env.PioPlatform()  # noqa: F821


def _key_from_sdkconfig_defaults():
    path = os.path.join(PROJECT_DIR, "sdkconfig.defaults")
    if not os.path.isfile(path):
        return ""
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            m = re.match(r'\s*CONFIG_SECURE_BOOT_SIGNING_KEY="(.+)"', line)
            if m:
                return m.group(1)
    return ""


def _find_signing_key():
    env_path = os.environ.get("OTA_SIGNING_KEY", "").strip()
    if env_path:
        if os.path.isfile(env_path):
            return env_path
        print("!! OTA_SIGNING_KEY=%s zeigt auf keine Datei, wird ignoriert." % env_path)
    cfg = _key_from_sdkconfig_defaults() or "keys/ota_signing_key.pem"
    candidate = cfg if os.path.isabs(cfg) else os.path.join(PROJECT_DIR, cfg)
    if os.path.isfile(candidate):
        return candidate
    return None


def _find_espsecure():
    pkg = platform.get_package_dir("tool-esptool") or ""
    for name in ("espsecure.exe", "espsecure"):
        p = os.path.join(pkg, name)
        if os.path.isfile(p):
            return p
    return shutil.which("espsecure") or shutil.which("espsecure.py")


SIGNING_KEY = _find_signing_key()
ESPSECURE = _find_espsecure()

# 1) Secure-Padding für das App-Image (wirkt auf den ElfToBin-Aufruf des Builders)
if "--secure-pad-v2" not in env.get("ELF2BINFLAGS", []):  # noqa: F821
    env.Append(ELF2BINFLAGS=["--secure-pad-v2"])  # noqa: F821


def sign_firmware(source, target, env):  # noqa: F811
    bin_path = str(target[0])
    if not SIGNING_KEY:
        print("")
        print("!! WARNUNG: Kein OTA-Signaturschlüssel gefunden (OTA_SIGNING_KEY oder keys/ota_signing_key.pem).")
        print("!!          %s bleibt UNSIGNIERT. Es bootet per USB, nimmt aber keine OTA-Updates an." % os.path.basename(bin_path))
        print("!!          Schlüssel erzeugen: tools/gen_signing_key.sh")
        print("")
        return
    if not ESPSECURE:
        sys.stderr.write("Fehler: espsecure nicht gefunden (Paket tool-esptool).\n")
        env.Exit(1)

    signed_path = bin_path[:-4] + "-signed.bin" if bin_path.endswith(".bin") else bin_path + ".signed"
    cmd = [ESPSECURE, "sign-data", "--version", "2", "--keyfile", SIGNING_KEY, "-o", signed_path, bin_path]
    print("Signiere %s mit %s" % (os.path.basename(bin_path), os.path.relpath(SIGNING_KEY, PROJECT_DIR)))
    rc = subprocess.call(cmd)
    if rc != 0:
        sys.stderr.write("Fehler: espsecure sign-data ist mit Code %d fehlgeschlagen.\n" % rc)
        env.Exit(1)

    # In-place ersetzen: upload und OTA-Upload verwenden dieselbe signierte Datei.
    shutil.move(signed_path, bin_path)
    subprocess.call([ESPSECURE, "signature-info-v2", bin_path])


env.AddPostAction(os.path.join("$BUILD_DIR", "${PROGNAME}.bin"), sign_firmware)  # noqa: F821
