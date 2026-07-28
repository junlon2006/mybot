# Native (Linux x86_64) toolchain for AOSL
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_FIND_ROOT_PATH "")
set(CONFIG_TOOLCHAIN_PREFIX "")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Use full paths to avoid CMake warnings
set(CMAKE_C_COMPILER   "/usr/bin/gcc")
set(CMAKE_CXX_COMPILER "/usr/bin/g++")
set(CMAKE_ASM_COMPILER "/usr/bin/as")
set(CMAKE_AR "/usr/bin/ar")
set(CMAKE_LD "/usr/bin/ld")
set(CMAKE_NM "/usr/bin/nm")
