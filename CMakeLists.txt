# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.13)

# The legacy "Sampler2" fork is kept in the repo (components/machine_sampler2)
# but dropped from the build — sampler3 has the hardware verdict (2026-07-18).
# Guarded so tools/proof_build.sh, which passes its own -DEXCLUDE_COMPONENTS,
# still overrides this for the stub-only proof build.
if(NOT DEFINED EXCLUDE_COMPONENTS)
    set(EXCLUDE_COMPONENTS machine_sampler2)
endif()

# pull in idf specific items, config vars etc.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Keep the build machine's home directory out of the images: assert()/ESP_LOG
# bake __FILE__ into .rodata, which put ~120 copies of the absolute source path
# (repo AND $IDF_PATH) into every app binary and 7 into the bootloader —
# including the recovery images archived under bin/ in the public repo (found
# 2026-09-05). Map $HOME to "~" so the strings read "~/esp/esp-idf-v4.3/...".
# Panic backtraces and assert messages still name the file; only the prefix
# changes. The app gets it via COMPILE_OPTIONS (below, after project()); the
# bootloader is a separate ExternalProject that only sees EXTRA_CMAKE_ARGS, so
# it gets a CMAKE_C_FLAGS override — which must repeat the toolchain's base
# flags (tools/cmake/toolchain-esp32.cmake in IDF 4.3: "-mlongcalls
# -Wno-frame-address") because the toolchain file's CACHE set() yields to a
# value given on the command line.
set(STRIP_HOME_FLAGS "-fmacro-prefix-map=$ENV{HOME}=~ -fdebug-prefix-map=$ENV{HOME}=~")
idf_build_set_property(EXTRA_CMAKE_ARGS
    "-DCMAKE_C_FLAGS=-mlongcalls -Wno-frame-address ${STRIP_HOME_FLAGS}" APPEND)

project(ctag-straempler)

idf_build_set_property(COMPILE_OPTIONS "-fmacro-prefix-map=$ENV{HOME}=~" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-fdebug-prefix-map=$ENV{HOME}=~" APPEND)

# generate include definitions for hw + fw version
#execute_process(
#        COMMAND bash -c "git describe --tags --abbrev=4"
#        OUTPUT_VARIABLE STRAMPLER_FW)
#string(STRIP ${STRAMPLER_FW} STRAMPLER_FW)
set(STRAMPLER_FW ${PROJECT_VER}) # uses idf derived project version (also from git)
configure_file(${CMAKE_SOURCE_DIR}/strampler_version.h.in ${CMAKE_BINARY_DIR}/gen_include/strampler_version.h @ONLY)
idf_build_set_property(INCLUDE_DIRECTORIES ${CMAKE_BINARY_DIR}/gen_include APPEND)
