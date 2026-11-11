# Cabinet fixtures

Run `python make_fixture.py` to regenerate deterministic stored/MSZIP and path-traversal fixtures using Python zlib. The second compression block uses data within the previous 32-KiB dictionary; it cannot be treated as an independent ZIP entry. `expected.bin` is the byte-for-byte oracle. On Windows the generator additionally calls Microsoft's makecab.exe to author `microsoft-mszip.cab`. No archived content is executed.

The reader currently rejects LZX, Quantum, cabinet-spanning files and CFDATA reserved extensions; those are not silently decompressed.
