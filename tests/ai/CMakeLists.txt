quarry_add_test(tst_ai_context SOURCES tst_ai_context.cpp LIBS quarry_ai)
quarry_add_test(tst_ai_sanitizer SOURCES tst_ai_sanitizer.cpp LIBS quarry_ai)
quarry_add_test(tst_claude_cli SOURCES tst_claude_cli.cpp LIBS quarry_ai)
quarry_add_test(tst_script_diff SOURCES tst_script_diff.cpp LIBS quarry_ai)
quarry_add_test(tst_ai_architecture SOURCES tst_ai_architecture.cpp LIBS quarry_ai)
target_compile_definitions(tst_ai_architecture PRIVATE
    QUARRY_AI_SOURCE_DIR="${PROJECT_SOURCE_DIR}/src/ai")
