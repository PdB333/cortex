# Native ELF fixtures

`make_fixture.py` creates ELF32/64 in both byte orders and an extended-count variant. Expected symbols, relocations, GNU build ID, dynamic dependency and hardening flags are specified in the generator. The existing `../elf/actor.elf`, built by GCC from owned C source, provides a separate real compiler fixture. No ELF file is executed by these tests.

Tests inventory bytes and metadata; they do not establish live Linux support, universal architecture disassembly or whole-program protection.
