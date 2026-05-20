set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH
    /usr/x86_64-w64-mingw32
    $ENV{HOME}/mingw-deps/SDL2-2.30.7/x86_64-w64-mingw32
    $ENV{HOME}/mingw-deps/SDL2_ttf-2.22.0/x86_64-w64-mingw32
)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(WIN32 TRUE)
set(MINGW TRUE)

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++ -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive,-Bdynamic")

# The prebuilt MSVC ffmpeg static libs (libavutil etc.) reference the bare
# CRT symbols `snprintf`, `vsnprintf`, `sscanf`, `fprintf`. They're meant to
# be satisfied by MSVC's UCRT static lib at link time. mingw's libmsvcrt does
# contain these, but they live in archive members (e.g. timecode.o in
# libavutil) with their OWN unresolvable __imp_ deps that poison the lookup.
#
# The cleanest fix: tell the linker to redirect every reference to these
# names to mingw's own `__mingw_*` variants in libmingwex.a. --defsym creates
# an alias at link time without any source-level redefinition — and mingw's
# <stdio.h> declares these as `inline`, which would otherwise block redefs.
set(_STDIO_DEFSYM
    "-Wl,--defsym=snprintf=__mingw_snprintf \
     -Wl,--defsym=vsnprintf=__mingw_vsnprintf \
     -Wl,--defsym=sscanf=__mingw_sscanf \
     -Wl,--defsym=fprintf=__mingw_fprintf")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${_STDIO_DEFSYM} -lmingwex")

# Case-correcting symlink overlay for capitalized Windows headers (Windows.h etc).
# PPSSPP source uses MSVC-style capitalization that fails on case-sensitive Linux.
include_directories(BEFORE SYSTEM "/home/teo/mingw-include-override")
add_compile_options("-I/home/teo/mingw-include-override")
