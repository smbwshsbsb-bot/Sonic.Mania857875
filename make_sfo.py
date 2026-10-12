#!/usr/bin/env python3
"""Generate sce_sys/param.sfo for the m7mddz PS4 homebrew app."""
import os, struct

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = os.path.join(root, "sce_sys", "param.sfo")

TITLE = "m7mddz"
TITLE_ID = "MDDZ00001"
CONTENT_ID = "IV0000-MDDZ00001_00-M7MDDZFILEMGR000"

# (key, kind, value, max_len)   kind: "s" = utf8 string, "i" = uint32
entries = [
    ("APP_TYPE", "i", 0, 4),
    ("APP_VER", "s", "01.00", 8),
    ("ATTRIBUTE", "i", 0, 4),
    ("CATEGORY", "s", "gd", 4),
    ("CONTENT_ID", "s", CONTENT_ID, 48),
    ("DOWNLOAD_DATA_SIZE", "i", 0, 4),
    ("FORMAT", "s", "obs", 4),
    ("PARENTAL_LEVEL", "i", 1, 4),
    ("SYSTEM_VER", "i", 0, 4),
    ("TITLE", "s", TITLE, 128),
    ("TITLE_ID", "s", TITLE_ID, 12),
    ("VERSION", "s", "01.00", 8),
]
entries.sort(key=lambda e: e[0])  # SFO keys must be sorted

key_table = b""
data_table = b""
index = b""
for key, kind, val, maxlen in entries:
    koff = len(key_table)
    key_table += key.encode() + b"\0"
    doff = len(data_table)
    if kind == "s":
        raw = val.encode() + b"\0"
        dlen = len(raw)
        fmt = 0x0204
        raw = raw.ljust(maxlen, b"\0")
    else:
        raw = struct.pack("<I", val)
        dlen = 4
        fmt = 0x0404
    data_table += raw
    index += struct.pack("<HHIII", koff, fmt, dlen, len(raw), doff)

while len(key_table) % 4:
    key_table += b"\0"

count = len(entries)
key_off = 20 + count * 16
data_off = key_off + len(key_table)
header = struct.pack("<4sIIII", b"\0PSF", 0x00000101, key_off, data_off, count)

os.makedirs(os.path.dirname(out), exist_ok=True)
with open(out, "wb") as f:
    f.write(header + index + key_table + data_table)
print("wrote", out)
