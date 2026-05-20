zstd single-file library — required for RSB1 bytecode compression.

1. Go to: https://github.com/facebook/zstd/releases/latest
2. Download the source .tar.gz
3. Extract: zstd-*/build/single_file_libs/zstd.h
             zstd-*/build/single_file_libs/zstdlib.c
4. Place both files in this folder (deps/zstd/)
5. Rename zstdlib.c -> zstd.c

No install or build step required — it compiles as part of the project.
