#[=======================================================================[
SPDX-License-Identifier: GPL-2.0-only
SPDX-FileCopyrightText: © 2026 Shaheem Azmal M MD
#]=======================================================================]

set(FO_CWD ${CMAKE_CURRENT_SOURCE_DIR})

# Both suites need the model the agent was built against; without one they
# would pass by finding nothing, so they are skipped rather than run blind.
if(DEFINED THESMO_MODEL_DIR AND EXISTS "${THESMO_MODEL_DIR}/model.json")
    add_test(NAME thesmo_functional_cli_test
        COMMAND bash ${FO_CWD}/Functional/cli_test.sh ${THESMO_MODEL_DIR}
        WORKING_DIRECTORY ${FO_CWD}/Functional)
    set_tests_properties(thesmo_functional_cli_test PROPERTIES
        ENVIRONMENT "THESMO=$<TARGET_FILE:thesmo>")
else()
    message(WARNING "thesmo: no model, so the functional tests are not registered")
endif()

# The database checks say what the schema does and do not need the agent; they
# skip themselves when no database is reachable.
add_test(NAME thesmo_db_test
    COMMAND bash ${FO_CWD}/Functional/db_test.sh
    WORKING_DIRECTORY ${FO_CWD}/Functional)

# Every name the agent can report needs a row in the shipped seed data, or the
# finding is skipped and nothing says so. Reads files only, so it needs no
# database; it needs the model to know what the names are.
if(DEFINED THESMO_MODEL_DIR AND EXISTS "${THESMO_MODEL_DIR}/model.json")
    add_test(NAME thesmo_vocabulary_test
        COMMAND bash ${FO_CWD}/Functional/vocabulary_test.sh ${THESMO_MODEL_DIR}
        WORKING_DIRECTORY ${FO_CWD}/Functional)
endif()

# The write policy is linked against stubs so each statement can be made to
# fail on demand: the states that matter are the ones a database will not
# reach on request. No database and no model, so this always runs.
add_executable(test_thesmo_commit ${FO_CWD}/Unit/test_commit.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../agent/thesmo_commit.c)
target_include_directories(test_thesmo_commit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../agent
    ${FO_CLIB_SRC}
    ${glib_INCLUDE_DIRS}
    ${PostgreSQL_INCLUDE_DIRS})
add_test(NAME thesmo_commit_test COMMAND test_thesmo_commit)
