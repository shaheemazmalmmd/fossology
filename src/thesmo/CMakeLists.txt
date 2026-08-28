#[=======================================================================[
SPDX-License-Identifier: GPL-2.0-only
SPDX-FileCopyrightText: © 2026 Shaheem Azmal M MD
#]=======================================================================]

cmake_minimum_required(VERSION 3.18 FATAL_ERROR)
project(thesmo LANGUAGES C)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SetDefaults.cmake)

# The model is a build artifact and is not in this repository: it is produced
# by the Thesmo trainer and pinned by a manifest. Only an explicit location is
# honoured -- an environment variable, -DTHESMO_MODEL_DIR, or a model/
# directory beside this one. Probing a sibling checkout would make the build
# depend on what happens to sit next to it.
set(THESMO_MODEL_DIR "" CACHE PATH
    "Directory holding the Thesmo model.json; also read from the environment")
if(NOT THESMO_MODEL_DIR)
    foreach(_candidate "$ENV{THESMO_MODEL_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/model")
        if(_candidate AND EXISTS "${_candidate}/model.json")
            get_filename_component(_found "${_candidate}" ABSOLUTE)
            set(THESMO_MODEL_DIR "${_found}" CACHE PATH "" FORCE)
            break()
        endif()
    endforeach()
endif()
if(DEFINED THESMO_MODEL_DIR AND EXISTS "${THESMO_MODEL_DIR}/model.json")
    message(STATUS "thesmo: installing model from ${THESMO_MODEL_DIR}")
else()
    message(WARNING
        "thesmo: no model found, so the agent will install but refuse to run.\n"
        "  cmake -DTHESMO_MODEL_DIR=/path/to/model ...   (cached, set it once)\n"
        "  or export THESMO_MODEL_DIR=/path/to/model     (every build tree)\n"
        "  or put the model in src/thesmo/model/.\n"
        "  The directory is the one holding model.json.")
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/agent)
if(TESTING)
    enable_testing()
    add_subdirectory(agent_tests)
endif()
configure_file(${FO_CMAKEDIR}/TestInstall.make.in ${CMAKE_CURRENT_BINARY_DIR}/TestInstall.make
    NEWLINE_STYLE LF @ONLY)
generate_version()

install(DIRECTORY ui
    DESTINATION ${FO_MODDIR}/${PROJECT_NAME}
    COMPONENT thesmo
    FILES_MATCHING
    PATTERN *.php)

install(FILES thesmo.conf "${CMAKE_CURRENT_BINARY_DIR}/VERSION"
    DESTINATION ${FO_MODDIR}/${PROJECT_NAME}
    COMPONENT thesmo)

install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/install/thesmo-seed.php
    DESTINATION ${FO_MODDIR}/${PROJECT_NAME}/install
    COMPONENT thesmo)

add_symlink()
