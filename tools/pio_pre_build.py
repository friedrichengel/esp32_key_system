"""
PlatformIO extra script (pre:) – bettet die Web-UI vor dem Build ein.

PlatformIO ruft CMake nur für das Code-Modell auf und bemerkt geänderte Web-Dateien
nicht zuverlässig. Deshalb wird tools/embed_web_assets.py hier zusätzlich ausgeführt
(idf.py erledigt das über execute_process in main/CMakeLists.txt).
"""
import os
import subprocess
import sys

Import("env")  # noqa: F821

PROJECT_DIR = env["PROJECT_DIR"]  # noqa: F821
WEB_DIR = os.path.join(PROJECT_DIR, "main", "web")
OUT = os.path.join(PROJECT_DIR, "main", "web_assets_gen.h")
FILES = [os.path.join(WEB_DIR, n) for n in ("index.html", "app.js", "style.css", "qrcode.js")]

rc = subprocess.call([sys.executable, os.path.join(PROJECT_DIR, "tools", "embed_web_assets.py"), OUT] + FILES)
if rc != 0:
    sys.stderr.write("Fehler: Web-Assets konnten nicht eingebettet werden.\n")
    env.Exit(1)  # noqa: F821
