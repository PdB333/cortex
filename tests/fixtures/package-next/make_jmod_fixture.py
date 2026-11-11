#!/usr/bin/env python3
"""Create a real JMOD with a JDK; compile fixture code but never execute it."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
javac, jmod = shutil.which('javac'), shutil.which('jmod')
if not javac or not jmod:
    raise SystemExit('A JDK is required to regenerate the committed jdk.jmod fixture')
with tempfile.TemporaryDirectory(prefix='cortex-jmod-fixture-') as tmp:
    root = Path(tmp)
    source = root / 'src'
    (source / 'fixture').mkdir(parents=True)
    (source / 'module-info.java').write_text('module cortex.fixture { exports fixture; }\n', encoding='utf-8')
    (source / 'fixture' / 'Example.java').write_text('package fixture; public final class Example { public static int value() { return 42; } }\n', encoding='utf-8')
    classes = root / 'classes'
    subprocess.run([javac, '-d', str(classes), str(source / 'module-info.java'), str(source / 'fixture' / 'Example.java')], check=True)
    target = root / 'jdk.jmod'
    subprocess.run([jmod, 'create', '--class-path', str(classes), '--module-version', '1.2.3', str(target)], check=True)
    result = subprocess.run([jmod, 'describe', str(target)], check=True, capture_output=True, text=True, encoding='utf-8')
    assert 'cortex.fixture@1.2.3' in result.stdout and 'exports fixture' in result.stdout
    shutil.copyfile(target, HERE / 'jdk.jmod')
    (HERE / 'jdk-jmod-expected.txt').write_text(result.stdout.rstrip() + '\n', encoding='utf-8')
    print(result.stdout)
