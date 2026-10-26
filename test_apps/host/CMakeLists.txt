cmake_minimum_required(VERSION 3.16)
project(cft_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(OpenSSL REQUIRED)
find_library(NGHTTP2_LIB nghttp2 REQUIRED)

set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_library(cf_tunnel STATIC
    ${CORE_DIR}/src/cft_capnp.c
    ${CORE_DIR}/src/cft_conn.c
    ${CORE_DIR}/src/cft_rpc.c
    ${CORE_DIR}/src/cft_token.c
    ${CORE_DIR}/src/cft_util.c
)
target_include_directories(cf_tunnel PUBLIC ${CORE_DIR}/include PRIVATE ${CORE_DIR}/src)
target_link_libraries(cf_tunnel PUBLIC ${NGHTTP2_LIB})

add_executable(cft_host main.c)
target_link_libraries(cft_host cf_tunnel OpenSSL::SSL OpenSSL::Crypto)

# RPC client test against mock_registration_server.py, which uses the
# reference Cap'n Proto implementation (pycapnp).
add_executable(rpc_test rpc_test.c)
target_include_directories(rpc_test PRIVATE ${CORE_DIR}/src)
target_link_libraries(rpc_test cf_tunnel)
