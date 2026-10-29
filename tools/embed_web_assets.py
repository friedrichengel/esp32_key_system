#!/usr/bin/env python3
"""
Erzeugt aus den Web-UI-Dateien einen C-Header mit gzip-komprimierten Byte-Arrays.

Aufruf:  embed_web_assets.py <ziel.h> <datei1> [<datei2> ...]

Je Datei entstehen  static const unsigned char web_<name>_gz[]  und
                    static const size_t        web_<name>_gz_len
wobei <name> aus dem Dateinamen abgeleitet wird (index.html -> index_html).

Wird aus main/CMakeLists.txt (execute_process, idf.py und PlatformIO-CMake-Codemodel)
und zusätzlich als PlatformIO-pre-Skript (tools/pio_pre_build.py) aufgerufen, damit
geänderte Web-Dateien in beiden Build-Wegen sicher neu eingebettet werden.
"""
import gzip
import os
import re
import sys


def symbol_name(path):
    base = os.path.basename(path)
    return "web_" + re.sub(r"[^A-Za-z0-9]", "_", base) + "_gz"


def main(argv):
    if len(argv) < 3:
        print("usage: embed_web_assets.py <out.h> <file> [...]", file=sys.stderr)
        return 2
    out_path, files = argv[1], argv[2:]

    # Nur neu schreiben, wenn eine Quelle neuer ist als der Header (vermeidet unnötige Rebuilds)
    if os.path.isfile(out_path):
        out_m = os.path.getmtime(out_path)
        if all(os.path.getmtime(f) <= out_m for f in files) and os.path.getmtime(__file__) <= out_m:
            return 0

    lines = [
        "/* AUTOMATISCH ERZEUGT von tools/embed_web_assets.py - nicht editieren, nicht einchecken. */",
        "#pragma once",
        "#include <stddef.h>",
        "",
    ]
    total_raw = total_gz = 0
    for f in files:
        with open(f, "rb") as fh:
            raw = fh.read()
        data = gzip.compress(raw, compresslevel=9, mtime=0)
        total_raw += len(raw)
        total_gz += len(data)
        sym = symbol_name(f)
        lines.append("/* %s: %d -> %d Byte */" % (os.path.basename(f), len(raw), len(data)))
        lines.append("static const unsigned char %s[] = {" % sym)
        for i in range(0, len(data), 16):
            chunk = data[i:i + 16]
            lines.append("    " + ", ".join("0x%02x" % b for b in chunk) + ",")
        lines.append("};")
        lines.append("static const size_t %s_len = sizeof(%s);" % (sym, sym))
        lines.append("")

    tmp = out_path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))
    os.replace(tmp, out_path)
    print("Web-Assets eingebettet: %d Dateien, %d -> %d Byte (%s)" % (len(files), total_raw, total_gz, os.path.basename(out_path)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
