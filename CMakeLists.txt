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

project(ctag-straempler)

# generate include definitions for hw + fw version
#execute_process(
#        COMMAND bash -c "git describe --tags --abbrev=4"
#        OUTPUT_VARIABLE STRAMPLER_FW)
#string(STRIP ${STRAMPLER_FW} STRAMPLER_FW)
set(STRAMPLER_FW ${PROJECT_VER}) # uses idf derived project version (also from git)
configure_file(${CMAKE_SOURCE_DIR}/strampler_version.h.in ${CMAKE_BINARY_DIR}/gen_include/strampler_version.h @ONLY)
idf_build_set_property(INCLUDE_DIRECTORIES ${CMAKE_BINARY_DIR}/gen_include APPEND)
