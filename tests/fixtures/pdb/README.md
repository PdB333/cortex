# PDB fixtures

These files belong to Cortex's tests and are never executed.

`make_fixture.py` deterministically creates `synthetic-512.pdb` (x86 DBI)
and `synthetic-4096.pdb` (x64 DBI): MSF7, fragmented streams, GUID/age,
DBI modules/source files, public and private records, a Point type, and C13
lines. The generator follows the field layouts documented at
https://llvm.org/docs/PDB/ (see provider source references).

`sample-x64.pdb` was generated from the adjacent `sample.cpp` by Clang/lld
17 targeting Windows. This is a producer-independent fixture (Cortex did
not write its PDB records). Its SHA-256 is:

`1a1084acc5219437c8be9c3e5bb815fff50c145a47fdebf21e5a3345c75cb354`

Rebuild it with LLVM installed for test generation only:

```sh
clang++ --target=x86_64-pc-windows-msvc -O0 -g -gcodeview -fno-stack-protector -c sample.cpp -o sample-x64.obj
lld-link /entry:cortex_entry /subsystem:console /nodefaultlib /debug:full /pdb:sample-x64.pdb /out:sample-x64.exe sample-x64.obj
```

Do not run the resulting executable. Paths and GUIDs vary when rebuilt, so
tests assert the source-defined names, Point's size and line numbers, not a
path-dependent checksum. The parser itself has no LLVM, DIA or dbghelp dependency.

MSF 2, portable PDB, complete recursive type resolution, legacy C11 lines,
OMAP remapping and source-server execution are not implemented. Unknown
CodeView records are skipped by their validated length, not interpreted.
