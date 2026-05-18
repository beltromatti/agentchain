# Unit tests for AgentChain Engine.

add_executable(test_crypto test_crypto.c)
target_link_libraries(test_crypto PRIVATE agentchain_engine)
add_test(NAME crypto COMMAND test_crypto)

add_executable(test_codec test_codec.c)
target_link_libraries(test_codec PRIVATE agentchain_engine)
add_test(NAME codec COMMAND test_codec)

add_executable(test_state test_state.c)
target_link_libraries(test_state PRIVATE agentchain_engine)
add_test(NAME state COMMAND test_state)
