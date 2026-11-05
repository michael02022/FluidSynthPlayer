# Script mode (cmake -P): writes a binary file as a C++ source with the same
# symbols `xxd -i` would generate, without needing xxd (not available on
# Windows):
#   unsigned char <NAME>[] = {0x.., ...};
#   unsigned int <NAME>_len = <size>;
#
#   cmake -DINPUT=<file> -DOUTPUT=<file.cpp> -DNAME=<symbol> -P EmbedFile.cmake
foreach(var INPUT OUTPUT NAME)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "EmbedFile.cmake: ${var} not set")
  endif()
endforeach()

file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hexLength)
math(EXPR size "${hexLength} / 2")

# 16 bytes per line ("0x..," each); CMake regexes have no {n} quantifier.
set(byte "[0-9a-f][0-9a-f]")
set(line "")
foreach(i RANGE 1 16)
  string(APPEND line "${byte}")
endforeach()
string(REGEX REPLACE "(${line})" "\\1\n" hex "${hex}")
string(REGEX REPLACE "(${byte})" "0x\\1," hex "${hex}")

file(WRITE "${OUTPUT}"
  "// Generated from ${INPUT} by cmake/EmbedFile.cmake - do not edit.\n"
  "unsigned char ${NAME}[] = {\n${hex}};\n"
  "unsigned int ${NAME}_len = ${size};\n")
