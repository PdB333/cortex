#!/usr/bin/env python3
"""Builds Cortex.Fixture.jar from src/ with javac --release 17 (committed so
the tests run without a JDK); --out DIR builds a fresh copy elsewhere."""
import argparse, os, shutil, subprocess, tempfile, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=HERE)
    args = parser.parse_args()
    classes = tempfile.mkdtemp(prefix="cortex-java-fixture-")
    try:
        sources = [os.path.join(root, name) for root, _, names in os.walk(os.path.join(HERE, "src")) for name in names if name.endswith(".java")]
        subprocess.run(["javac", "--release", "17", "-d", classes] + sorted(sources), check=True)
        os.makedirs(args.out, exist_ok=True)
        jar = os.path.join(args.out, "Cortex.Fixture.jar")
        # Fixed timestamps and order: the jar's bytes depend only on javac's output.
        with zipfile.ZipFile(jar, "w", zipfile.ZIP_DEFLATED) as out:
            info = zipfile.ZipInfo("META-INF/MANIFEST.MF", (2026, 1, 1, 0, 0, 0))
            out.writestr(info, "Manifest-Version: 1.0\r\nCreated-By: make_java_fixture.py\r\n\r\n")
            for root, _, names in sorted(os.walk(classes)):
                for name in sorted(names):
                    path = os.path.join(root, name)
                    info = zipfile.ZipInfo(os.path.relpath(path, classes).replace(os.sep, "/"), (2026, 1, 1, 0, 0, 0))
                    info.compress_type = zipfile.ZIP_DEFLATED
                    out.writestr(info, open(path, "rb").read())
        print(jar)
    finally:
        shutil.rmtree(classes, ignore_errors=True)

if __name__ == "__main__":
    main()
