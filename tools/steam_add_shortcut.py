"""Add (or update) a non-Steam game shortcut in Steam's binary shortcuts.vdf.

python -I tools/steam_add_shortcut.py <shortcuts.vdf> <AppName> <exe> <startdir> [launch options] [icon]
Steam must not be running (it rewrites the file on exit). A .bak copy is written first.
"""
import os
import shutil
import struct
import sys
import zlib


def parse(data, i):
    out = []
    while True:
        t = data[i]
        i += 1
        if t == 0x08:
            return out, i
        j = data.index(b"\0", i)
        key = data[i:j].decode("utf-8", "replace")
        i = j + 1
        if t == 0x00:
            val, i = parse(data, i)
        elif t == 0x01:
            j = data.index(b"\0", i)
            val = data[i:j].decode("utf-8", "replace")
            i = j + 1
        elif t == 0x02:
            val = struct.unpack_from("<i", data, i)[0]
            i += 4
        else:
            raise ValueError(f"unknown type {t:#x} at {i}")
        out.append([key, t, val])


def dump(items):
    b = bytearray()
    for key, t, val in items:
        b.append(t)
        b += key.encode() + b"\0"
        if t == 0x00:
            b += dump(val) + b"\x08"
        elif t == 0x01:
            b += val.encode() + b"\0"
        else:
            b += struct.pack("<i", val)
    return bytes(b)


def main():
    path, name, exe, startdir = sys.argv[1:5]
    launch = sys.argv[5] if len(sys.argv) > 5 else ""
    icon = sys.argv[6] if len(sys.argv) > 6 else ""
    if os.path.exists(path):
        data = open(path, "rb").read()
        shutil.copyfile(path, path + ".bak")
        root, _ = parse(data, 0)
    else:
        root = [["shortcuts", 0, []]]
    shortcuts = root[0][2]
    qexe = f'"{exe}"'
    appid = (zlib.crc32((qexe + name).encode()) | 0x80000000) - (1 << 32)
    entry = [
        ["appid", 2, appid], ["AppName", 1, name], ["Exe", 1, qexe], ["StartDir", 1, f'"{startdir}"'],
        ["icon", 1, icon], ["ShortcutPath", 1, ""], ["LaunchOptions", 1, launch], ["IsHidden", 2, 0],
        ["AllowDesktopConfig", 2, 1], ["AllowOverlay", 2, 1], ["OpenVR", 2, 0], ["Devkit", 2, 0],
        ["DevkitGameID", 1, ""], ["DevkitOverrideAppID", 2, 0], ["LastPlayTime", 2, 0],
        ["FlatpakAppID", 1, ""], ["tags", 0, []],
    ]
    for item in shortcuts:  # update existing entry with the same name instead of duplicating
        fields = {k: v for k, _, v in item[2]}
        if fields.get("AppName", fields.get("appname")) == name:
            item[2] = entry
            break
    else:
        shortcuts.append([str(len(shortcuts)), 0, entry])
    open(path, "wb").write(dump(root) + b"\x08")
    print(f"{len(shortcuts)} shortcut(s); '{name}' appid {appid & 0xFFFFFFFF}")


if __name__ == "__main__":
    main()
