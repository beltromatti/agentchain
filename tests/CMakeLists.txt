# Unit tests for AgentChain Engine.
#
# Tests use assert() for checks; we explicitly disable NDEBUG so Release
# builds (CI default) still evaluate the assertions.

set(AGENTCHAIN_TEST_DEFS UNDEF_NDEBUG)

function(agentchain_add_test name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE agentchain_engine)
    target_compile_options(${name} PRIVATE -UNDEBUG -Wno-error=unused-variable)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

agentchain_add_test(test_crypto)
agentchain_add_test(test_codec)
agentchain_add_test(test_state)
