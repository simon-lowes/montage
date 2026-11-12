# Montage — one of libvmaf's VMAF models (a JSON file) as the C array its built-in models are (what its build makes
# with xxd -i -n src_<name>, here without xxd): cmake -DIN=<model.json> -DOUT=<file.c> -P embed-model.cmake
get_filename_component(name "${IN}" NAME)
string(MAKE_C_IDENTIFIER "src_${name}" id)
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" digits)
math(EXPR bytes "${digits} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," body "${hex}")
string(REGEX REPLACE "((0x..,){24})" "\\1\n" body "${body}")
file(WRITE "${OUT}" "/* ${name}, from libvmaf's models */\nconst char ${id}[] = {\n${body}\n};\nconst int ${id}_len = ${bytes};\n")
