# APPX/MSIX metadata fixtures

`make_fixture.py` uses ElementTree, zipfile and hashlib to author namespace-qualified manifests and independent SHA-256 block expectations. It creates a normal package, UTF-16 manifest, valid-ZIP-CRC/tampered-block package, forbidden-DTD package and nested bundle. None is installed or executed.

These are unsigned structural fixtures, not deployable packages or a complete Microsoft SDK conformance suite. Hash agreement with an untrusted block map is not signature verification.
