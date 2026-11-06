# PE seeds

`tiny64.exe` and `tiny86.exe` are built from `tiny.c` with no C runtime, so
they stay a few kilobytes and the PE/native-code fuzzer spends its time on
the parser rather than on decoding a large binary:

    x86_64-w64-mingw32-gcc -O1 -nostdlib -e entry -s tiny.c -o tiny64.exe
    i686-w64-mingw32-gcc -O1 -nostdlib -e _entry -s tiny.c -o tiny86.exe
