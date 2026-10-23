# Windows x86-64 from Linux with llvm-mingw (https://github.com/mstorsjo/llvm-mingw).
# cmake -S engine -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=engine/cmake/toolchains/llvm-mingw-x86_64.cmake
# LLVM_MINGW (default /opt/llvm-mingw) points at the toolchain.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
if(NOT LLVM_MINGW)
    set(LLVM_MINGW $ENV{LLVM_MINGW})
endif()
if(NOT LLVM_MINGW)
    set(LLVM_MINGW /opt/llvm-mingw)
endif()
set(TRIPLE x86_64-w64-mingw32)
set(CMAKE_C_COMPILER ${LLVM_MINGW}/bin/${TRIPLE}-clang)
set(CMAKE_CXX_COMPILER ${LLVM_MINGW}/bin/${TRIPLE}-clang++)
set(CMAKE_RC_COMPILER ${LLVM_MINGW}/bin/${TRIPLE}-windres)
set(CMAKE_AR ${LLVM_MINGW}/bin/${TRIPLE}-ar)
set(CMAKE_FIND_ROOT_PATH ${LLVM_MINGW}/${TRIPLE})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# One self-contained .exe: the C++ runtime and winpthreads linked in.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
