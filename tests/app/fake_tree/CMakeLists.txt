# Test-only engine with several databases, schemas, users and indexes, for tst_schema_tree_model.
# Its own directory: the other tests that scan a plugin directory must not see it.
set(QUARRY_FAKE_TREE_PLUGIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/tests/plugins_fake_tree" CACHE INTERNAL "")
quarry_add_provider(fake_tree TEST_ONLY SOURCES fake_tree_plugin.cpp fake_tree.json)
set_target_properties(quarry_provider_fake_tree PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${QUARRY_FAKE_TREE_PLUGIN_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${QUARRY_FAKE_TREE_PLUGIN_OUTPUT_DIR}")
