# quarry_ai: the Claude Code integration (H13). Isolated on purpose: it links only Qt6::Core and
# never quarry_core or quarry_provider_api, so it cannot reach result types, sessions or query
# execution. It only sees the metadata-only SchemaContext (ARCHITECTURE §6.13).
qt_add_library(quarry_ai STATIC
    include/quarry/ai/schema_context.h
    include/quarry/ai/context_builder.h
    include/quarry/ai/ai_sanitizer.h
    include/quarry/ai/claude_cli.h
    include/quarry/ai/ai_prompt.h
    include/quarry/ai/script_diff.h
    src/context_builder.cpp
    src/ai_sanitizer.cpp
    src/claude_cli.cpp
    src/ai_prompt.cpp
    src/script_diff.cpp
)
target_include_directories(quarry_ai PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(quarry_ai PUBLIC Qt6::Core)
quarry_set_warnings(quarry_ai)
quarry_assert_ai_isolated(quarry_ai)
