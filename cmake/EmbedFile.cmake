# Turns a file into a C source that defines it as a byte array, so the file
# travels inside the binary.
#
#   cmake -DINPUT=<file> -DOUTPUT=<file.c> -DSYMBOL=<name> -P EmbedFile.cmake
#
# Defines `const unsigned char <name>[]` (NUL-terminated for convenience) and
# `const size_t <name>_len` (the length without that NUL).

file(READ "${INPUT}" hex HEX)
# 16 bytes per line keeps the generated file readable and the lines short.
string(REGEX REPLACE "(................................)" "\\1\n" hex "${hex}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUTPUT}"
  "/* Generated from ${INPUT}. Do not edit. */\n"
  "#include <stddef.h>\n"
  "extern const unsigned char ${SYMBOL}[];\n"
  "extern const size_t ${SYMBOL}_len;\n"
  "const unsigned char ${SYMBOL}[] = {\n${bytes}\n0x00};\n"
  "const size_t ${SYMBOL}_len = sizeof ${SYMBOL} - 1;\n")
