quarry_add_test(tst_ai_context SOURCES tst_ai_context.cpp LIBS quarry_ai)
quarry_add_test(tst_ai_sanitizer SOURCES tst_ai_sanitizer.cpp LIBS quarry_ai)
quarry_add_test(tst_claude_cli SOURCES tst_claude_cli.cpp LIBS quarry_ai)
# A stand-in for `claude` (the real one is never run by any test), like tests/core's fake_php.
qt_add_executable(fake_claude fake_claude/fake_claude.cpp)
target_link_libraries(fake_claude PRIVATE Qt6::Core)
quarry_set_warnings(fake_claude)
quarry_add_test(tst_claude_runner SOURCES tst_claude_runner.cpp LIBS quarry_ai)
target_compile_definitions(tst_claude_runner PRIVATE QUARRY_FAKE_CLAUDE="$<TARGET_FILE:fake_claude>")
add_dependencies(tst_claude_runner fake_claude)
# Starts about thirty processes and waits for a few kills.
set_tests_properties(tst_claude_runner PROPERTIES TIMEOUT 180)
quarry_add_test(tst_script_diff SOURCES tst_script_diff.cpp LIBS quarry_ai)
quarry_add_test(tst_ai_architecture SOURCES tst_ai_architecture.cpp LIBS quarry_ai)
target_compile_definitions(tst_ai_architecture PRIVATE
    QUARRY_AI_SOURCE_DIR="${PROJECT_SOURCE_DIR}/src/ai")
