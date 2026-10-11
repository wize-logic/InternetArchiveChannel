# Converts a text file into a NUL-terminated C array (mbedTLS PEM parsing needs the NUL).
# cmake -DIN=<file> -DOUT=<file.c> -DSYM=<name> -P embed_text.cmake
file(READ "${IN}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(LENGTH "${hex}" hexlen)
math(EXPR size "${hexlen} / 2 + 1")
file(WRITE "${OUT}"
	"#include <stddef.h>\n"
	"const unsigned char ${SYM}[] = {${bytes}0x00};\n"
	"const size_t ${SYM}_size = ${size};\n")
