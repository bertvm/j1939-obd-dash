cmake_minimum_required(VERSION 3.22)

# Cursor/CMake Tools launches cmake without the ESP-IDF shell. Fill the EIM
# IDF 6.1 environment when those variables are missing so configure can proceed.
if(NOT DEFINED ENV{IDF_PATH} OR "$ENV{IDF_PATH}" STREQUAL "")
    set(ENV{IDF_PATH} "$ENV{HOME}/.espressif/v6.1/esp-idf")
endif()
if(NOT DEFINED ENV{IDF_TOOLS_PATH} OR "$ENV{IDF_TOOLS_PATH}" STREQUAL "")
    set(ENV{IDF_TOOLS_PATH} "$ENV{HOME}/.espressif/tools")
endif()
if(NOT DEFINED ENV{IDF_PYTHON_ENV_PATH} OR "$ENV{IDF_PYTHON_ENV_PATH}" STREQUAL "")
    set(ENV{IDF_PYTHON_ENV_PATH} "$ENV{IDF_TOOLS_PATH}/python/v6.1/venv")
endif()
if(EXISTS "$ENV{IDF_PYTHON_ENV_PATH}/bin")
    set(ENV{PATH} "$ENV{IDF_TOOLS_PATH}/cmake/4.0.3/CMake.app/Contents/bin:$ENV{IDF_TOOLS_PATH}/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin:$ENV{IDF_TOOLS_PATH}/ninja/1.12.1:$ENV{IDF_PYTHON_ENV_PATH}/bin:$ENV{PATH}")
endif()

if(NOT EXISTS "$ENV{IDF_PATH}/tools/cmake/project.cmake")
    message(FATAL_ERROR
        "ESP-IDF was not found at $ENV{IDF_PATH}. "
        "Install IDF 6.1 or run: . \"$ENV{HOME}/.espressif/tools/activate_idf_v6.1.sh\"")
endif()

# Keep CMake from reusing an older IDF virtualenv (those reject interface version 5).
set(_idf_python "$ENV{IDF_PYTHON_ENV_PATH}/bin/python")
if(EXISTS "${_idf_python}")
    set(ENV{PYTHON} "${_idf_python}")
    set(PYTHON "${_idf_python}" CACHE FILEPATH "ESP-IDF Python interpreter" FORCE)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(j1939_obd_dash)
