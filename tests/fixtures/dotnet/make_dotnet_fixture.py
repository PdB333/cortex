#!/usr/bin/env python3
"""Builds the .NET fixture with the real SDK and records reference answers.

  - Cortex.Fixture.dll        dotnet build -c Release (C#, known types/IL)
  - Cortex.Fixture.1.4.2.nupkg  dotnet pack (a ZIP with deflated entries)
  - zip-cases.zip             Python zipfile: stored, deflate levels 1 and 9,
                              an empty file, a directory, a UTF-8 name
  - Cortex.Fixture-expected.json  System.Reflection.Metadata answers and the
                              runtime's OpCodes table (Oracle/Program.cs)

    python tests/fixtures/dotnet/make_dotnet_fixture.py [--out DIR]

Without --out the committed fixtures are rewritten. CI writes them to a
temporary directory and checks Cortex against the fresh build.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

here = os.path.dirname(os.path.abspath(__file__))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=here)
    out = os.path.abspath(parser.parse_args().out)
    os.makedirs(out, exist_ok=True)
    env = dict(os.environ, DOTNET_CLI_TELEMETRY_OPTOUT="1", DOTNET_NOLOGO="1")
    work = tempfile.mkdtemp(prefix="cortex-dotnet-")
    try:
        project = os.path.join(here, "Fixture")
        subprocess.run(["dotnet", "build", project, "-c", "Release", "-o", os.path.join(work, "bin")], check=True, env=env,
                       stdout=subprocess.DEVNULL)
        subprocess.run(["dotnet", "pack", project, "-c", "Release", "-o", os.path.join(work, "pkg")], check=True, env=env,
                       stdout=subprocess.DEVNULL)
        dll = os.path.join(out, "Cortex.Fixture.dll")
        shutil.copyfile(os.path.join(work, "bin", "Cortex.Fixture.dll"), dll)
        shutil.copyfile(os.path.join(work, "pkg", "Cortex.Fixture.1.4.2.nupkg"), os.path.join(out, "Cortex.Fixture.1.4.2.nupkg"))
        with open(os.path.join(out, "Cortex.Fixture-expected.json"), "w", newline="\n") as expected:
            subprocess.run(["dotnet", "run", "--project", os.path.join(here, "Oracle"), "-c", "Release", "--", dll],
                           check=True, env=env, stdout=expected)
        text = ("The quick brown fox jumps over the lazy dog. " * 200).encode()
        with zipfile.ZipFile(os.path.join(out, "zip-cases.zip"), "w") as archive:
            archive.writestr(zipfile.ZipInfo("stored.txt"), text, compress_type=zipfile.ZIP_STORED)
            archive.writestr("fast.txt", text, compress_type=zipfile.ZIP_DEFLATED, compresslevel=1)
            archive.writestr("best.txt", text, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
            archive.writestr("random.bin", bytes((i * 7919 + 13) % 251 for i in range(5000)),
                             compress_type=zipfile.ZIP_DEFLATED)
            archive.writestr("empty.txt", b"", compress_type=zipfile.ZIP_DEFLATED)
            archive.writestr("folder/", b"")
            archive.writestr("folder/café.txt", "café".encode(), compress_type=zipfile.ZIP_DEFLATED)
            archive.comment = b"cortex zip cases"
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("fixture written to", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
