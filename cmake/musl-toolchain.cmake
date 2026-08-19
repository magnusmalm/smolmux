# CMake toolchain file for musl-static builds (native and cross-compilation).
# Auto-builds musl toolchain (via musl-cross-make) if not already present.
#
# smolmux has zero external library deps, so only the cross-compiler is needed.
#
# Usage:
#   cmake --preset musl              # Native x86_64
#   cmake --preset musl-aarch64      # Cross-compile for aarch64
#   cmake --preset musl-armv7        # Cross-compile for armv7l
#
# Or manually:
#   cmake -B build-musl -DSM_MUSL_STATIC=ON -DTARGET_ARCH=aarch64 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/musl-toolchain.cmake

# -- Determine target architecture -------------------------------------------

if(NOT DEFINED TARGET_ARCH)
    execute_process(COMMAND uname -m OUTPUT_VARIABLE TARGET_ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()

# -- Map arch to musl triple -------------------------------------------------

if(TARGET_ARCH STREQUAL "x86_64")
    set(_MUSL_TRIPLE "x86_64-linux-musl")
    set(CMAKE_SYSTEM_PROCESSOR "x86_64")
elseif(TARGET_ARCH STREQUAL "aarch64")
    set(_MUSL_TRIPLE "aarch64-linux-musl")
    set(CMAKE_SYSTEM_PROCESSOR "aarch64")
elseif(TARGET_ARCH STREQUAL "armv7l")
    set(_MUSL_TRIPLE "armv7l-linux-musleabihf")
    set(CMAKE_SYSTEM_PROCESSOR "armv7l")
else()
    message(FATAL_ERROR "Unsupported TARGET_ARCH: ${TARGET_ARCH} (expected x86_64, aarch64, armv7l)")
endif()

# -- Paths -------------------------------------------------------------------

set(_TOOLCHAIN_DIR "${CMAKE_CURRENT_LIST_DIR}/../deps/musl-toolchain-${TARGET_ARCH}")
set(_MUSL_CC "${_TOOLCHAIN_DIR}/bin/${_MUSL_TRIPLE}-gcc")
set(_BUILD_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../scripts/build_musl_toolchain.sh")

# -- Auto-build if toolchain is missing --------------------------------------

if(NOT EXISTS "${_MUSL_CC}")
    message(STATUS "musl toolchain not found for ${TARGET_ARCH} — building...")
    execute_process(
        COMMAND "${_BUILD_SCRIPT}" "${TARGET_ARCH}"
        RESULT_VARIABLE _build_result
    )
    if(NOT _build_result EQUAL 0)
        message(FATAL_ERROR "scripts/build_musl_toolchain.sh ${TARGET_ARCH} failed (exit ${_build_result})")
    endif()
    if(NOT EXISTS "${_MUSL_CC}")
        message(FATAL_ERROR "Compiler still not found after running build script: ${_MUSL_CC}")
    endif()
endif()

# -- Set cross-compilation variables -----------------------------------------

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_C_COMPILER "${_MUSL_CC}")
set(CMAKE_AR "${_TOOLCHAIN_DIR}/bin/${_MUSL_TRIPLE}-ar" CACHE FILEPATH "Archiver")
set(CMAKE_RANLIB "${_TOOLCHAIN_DIR}/bin/${_MUSL_TRIPLE}-ranlib" CACHE FILEPATH "Ranlib")
set(CMAKE_STRIP "${_TOOLCHAIN_DIR}/bin/${_MUSL_TRIPLE}-strip" CACHE FILEPATH "Strip")

set(CMAKE_FIND_ROOT_PATH "${_TOOLCHAIN_DIR}/${_MUSL_TRIPLE}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
