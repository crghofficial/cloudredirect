# Build on a modern host against the isolated glibc-2.31/GCC-12 sysroot.
# scripts/build-linux64.sh provisions the exact dependencies without installing
# any packages into the host system.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
get_filename_component(CR_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(CR_SYSROOT "${CR_ROOT}/build-deps/sysroot" CACHE PATH "Steam-compatible sysroot")
set(CMAKE_SYSROOT "${CR_SYSROOT}")
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_FLAGS_INIT "--gcc-toolchain=${CR_SYSROOT}/usr")
set(CMAKE_CXX_FLAGS_INIT "--gcc-toolchain=${CR_SYSROOT}/usr")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libstdc++ -Wl,--exclude-libs,ALL")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++")
set(CMAKE_FIND_ROOT_PATH "${CR_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
