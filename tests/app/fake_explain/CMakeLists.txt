# Test-only engine with Explain analyze, for tst_explain_controller. Its own directory: the other
# tests that scan a plugin directory must not see it.
set(QUARRY_FAKE_EXPLAIN_PLUGIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/tests/plugins_fake_explain" CACHE INTERNAL "")
quarry_add_provider(fake_explain TEST_ONLY SOURCES fake_explain_plugin.cpp fake_explain.json)
set_target_properties(quarry_provider_fake_explain PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${QUARRY_FAKE_EXPLAIN_PLUGIN_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${QUARRY_FAKE_EXPLAIN_PLUGIN_OUTPUT_DIR}")
