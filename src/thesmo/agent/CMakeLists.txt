#[=======================================================================[
SPDX-License-Identifier: GPL-2.0-only
SPDX-FileCopyrightText: © 2026 Shaheem Azmal M MD
#]=======================================================================]

set(FO_CWD ${CMAKE_CURRENT_SOURCE_DIR})
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${FO_C_FLAGS}")

include_directories(
    ${glib_INCLUDE_DIRS}
    ${PostgreSQL_INCLUDE_DIRS}
    ${FO_CLIB_SRC})

# building this module on its own, outside the top-level build
if(NOT TARGET fossology)
    add_subdirectory(${FO_CLIB_SRC} buildc EXCLUDE_FROM_ALL)
endif()

add_executable(thesmo "")
target_compile_definitions(thesmo
    PRIVATE
    _FILE_OFFSET_BITS=64
    DATADIR="${FO_MODDIR}"
    VERSION_S="${FO_VERSION}"
    COMMIT_HASH_S="${FO_COMMIT_HASH}")
target_sources(thesmo
    PRIVATE
    ${FO_CWD}/thesmo.c
    ${FO_CWD}/thesmo_model.c
    ${FO_CWD}/thesmo_db.c)
target_compile_definitions(thesmo PRIVATE FOSSOLOGY_AGENT=1)
target_link_libraries(thesmo
    PRIVATE fossology ${glib_LIBRARIES} ${PostgreSQL_LIBRARIES} m)

install(TARGETS thesmo
    RUNTIME DESTINATION ${FO_MODDIR}/${PROJECT_NAME}/agent
    COMPONENT thesmo)

# The model is discovered in the parent CMakeLists so the tests can see it too.
if(DEFINED THESMO_MODEL_DIR AND EXISTS "${THESMO_MODEL_DIR}/model.json")
    install(DIRECTORY ${THESMO_MODEL_DIR}/
        DESTINATION ${FO_MODDIR}/${PROJECT_NAME}/model
        COMPONENT thesmo)
endif()
