#!/usr/bin/env python3
"""Builds the fixtures of the reader of last resort (generic provider) in
tests/fixtures/format-more/generic, and records how libmagic's file(1)
identifies each one as the independent reference:

  media files from ffmpeg (PNG, JPEG, GIF, BMP, WebP, FLAC, Ogg, WAV, MP4,
  Matroska), xz/bzip2 data, text files of several languages, a UTF-16 text,
  a binary with known strings and three embedded files at known offsets
  (a PE from tests/fixtures/pe, a ZIP and a PNG), seeded random bytes, a
  low-entropy unknown game save and an empty file.

Needs ffmpeg, xz, bzip2, zip and file(1).
"""
import json
import os
import random
import struct
import subprocess
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "generic")
os.makedirs(OUT, exist_ok=True)
expected = {}


def ff(*args):
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", *args], check=True, timeout=120)


src_video = ["-f", "lavfi", "-i", "testsrc2=s=64x48:d=1:r=10"]
src_audio = ["-f", "lavfi", "-i", "sine=frequency=440:duration=0.5"]
ff(*src_video, "-frames:v", "1", os.path.join(OUT, "image.png"))
ff(*src_video, "-frames:v", "1", "-q:v", "5", os.path.join(OUT, "photo.jpg"))
ff(*src_video, "-frames:v", "1", os.path.join(OUT, "anim.gif"))
ff(*src_video, "-frames:v", "1", os.path.join(OUT, "picture.bmp"))
ff(*src_video, "-frames:v", "1", "-c:v", "libwebp", os.path.join(OUT, "picture.webp"))
ff(*src_audio, os.path.join(OUT, "tone.flac"))
ff(*src_audio, "-c:a", "libvorbis", os.path.join(OUT, "tone.ogg"))
ff(*src_audio, os.path.join(OUT, "tone.wav"))
ff(*src_video, "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", os.path.join(OUT, "clip.mp4"))
ff(*src_video, "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", os.path.join(OUT, "clip.mkv"))

text = ("Cortex generic fixture.\n" * 40).encode()
with open(os.path.join(OUT, "data.txt.xz"), "wb") as f:
    f.write(subprocess.run(["xz", "-c"], input=text, capture_output=True, check=True).stdout)
with open(os.path.join(OUT, "data.txt.bz2"), "wb") as f:
    f.write(subprocess.run(["bzip2", "-c"], input=text, capture_output=True, check=True).stdout)

texts = {
    "config.json": '{\n  "name": "cortex",\n  "version": 2,\n  "features": ["scan", "debug"]\n}\n',
    "page.html": "<!DOCTYPE html>\n<html><head><title>Cortex</title></head><body><p>owned</p></body></html>\n",
    "run.sh": "#!/bin/sh\necho owned fixture\nexit 0\n",
    "table.csv": "name,size,kind\nalpha,10,text\nbeta,20,binary\ngamma,30,text\n",
    "settings.ini": "[general]\nname=cortex\nmode=read-only\n\n[ui]\ntheme=dark\n",
    "notes.md": "# Cortex notes\n\nSome **owned** text.\n\n## Section\n\n- item\n",
    "export.reg": "Windows Registry Editor Version 5.00\r\n\r\n[HKEY_CURRENT_USER\\Software\\Cortex]\r\n\"Mode\"=\"read-only\"\r\n",
}
for name, content in texts.items():
    with open(os.path.join(OUT, name), "w", newline="") as f:
        f.write(content)
with open(os.path.join(OUT, "utf16.txt"), "wb") as f:
    f.write(b"\xff\xfe" + "Cortex UTF-16 text\r\nsecond line\r\n".encode("utf-16-le"))

# A binary with known strings and three files embedded at known offsets.
rng = random.Random(1234)
blob = bytearray(bytes(rng.randrange(0, 4) for _ in range(2048)))  # low-entropy filler without magics
strings_ascii = ["https://cortex.example/update?id=7", "contact: owner@cortex.example", "server 192.168.10.42 port 8080",
                 "C:\\Program Files\\Cortex\\cortex.exe", "HKEY_LOCAL_MACHINE\\SOFTWARE\\Cortex\\Settings",
                 "{3f2504e0-4f89-11d3-9a0c-0305e82c3301}", "/usr/lib/cortex/plugin.so"]
for s in strings_ascii:
    blob += b"\0\0" + s.encode() + b"\0\0"
blob += b"\0\0" + "http://utf16.cortex.example/path".encode("utf-16-le") + b"\0\0\0\0"
with open(os.path.join(HERE, "..", "pe", "signed-version.exe"), "rb") as f:
    pe = f.read()
pe_offset = len(blob)
blob += pe
blob += bytes(16)
with tempfile.TemporaryDirectory() as tmp:
    z = os.path.join(tmp, "inner.zip")
    with zipfile.ZipFile(z, "w") as archive:
        archive.writestr("inner/readme.txt", "owned inner file\n")
    with open(z, "rb") as f:
        zip_bytes = f.read()
zip_offset = len(blob)
blob += zip_bytes + bytes(16)
with open(os.path.join(OUT, "image.png"), "rb") as f:
    png = f.read()
png_offset = len(blob)
blob += png
with open(os.path.join(OUT, "dropper.bin"), "wb") as f:
    f.write(bytes(blob))

noise_rng = random.Random(99)  # one generator for all bytes
noise = bytes(noise_rng.randrange(256) for _ in range(65536))
with open(os.path.join(OUT, "noise.bin"), "wb") as f:
    f.write(noise)
with open(os.path.join(OUT, "slot1.sav"), "wb") as f:
    f.write(b"\x02\x00\x00\x00" + struct.pack("<I", 100) + bytes(range(16)) * 8 + b"\x00" * 256)
open(os.path.join(OUT, "empty.dat"), "wb").close()

for name in sorted(os.listdir(OUT)):
    if name == "expected.json":
        continue
    path = os.path.join(OUT, name)
    mime = subprocess.run(["file", "--brief", "--mime-type", path], capture_output=True, text=True).stdout.strip()
    desc = subprocess.run(["file", "--brief", path], capture_output=True, text=True).stdout.strip()
    expected[name] = {"file_mime": mime, "file_description": desc}
expected["dropper.bin"].update({"embedded": [pe_offset, zip_offset, png_offset], "strings": strings_ascii,
                                "utf16_url": "http://utf16.cortex.example/path"})
with open(os.path.join(OUT, "expected.json"), "w") as f:
    json.dump(expected, f, indent=1, sort_keys=True)
print("fixtures written to", OUT)
