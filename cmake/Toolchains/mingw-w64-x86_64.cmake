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

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")

# Case-correcting symlink overlay for capitalized Windows headers (Windows.h etc).
# PPSSPP source uses MSVC-style capitalization that fails on case-sensitive Linux.
include_directories(BEFORE SYSTEM "/home/teo/mingw-include-override")
add_compile_options("-I/home/teo/mingw-include-override")
