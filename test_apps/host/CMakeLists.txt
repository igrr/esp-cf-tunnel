cmake_minimum_required(VERSION 3.16)
project(cft_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CFT_WERROR "Treat warnings in this project's code as errors" OFF)

find_package(OpenSSL REQUIRED)

# nghttp2 >= 1.60 is needed (nghttp2_data_provider2 and friends). With
# CFT_FETCH_NGHTTP2=ON, build the version the ESP-IDF component uses.
option(CFT_FETCH_NGHTTP2 "Download and build nghttp2 instead of using the system library" OFF)
if(CFT_FETCH_NGHTTP2)
    include(FetchContent)
    set(ENABLE_LIB_ONLY ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
    set(ENABLE_DOC OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(nghttp2
        URL https://github.com/nghttp2/nghttp2/releases/download/v1.69.0/nghttp2-1.69.0.tar.xz)
    FetchContent_MakeAvailable(nghttp2)
    set(NGHTTP2_LIB nghttp2_static)
else()
    find_library(NGHTTP2_LIB nghttp2 REQUIRED)
endif()

function(cft_warnings target)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    if(CFT_WERROR)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()

set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_library(cf_tunnel STATIC
    ${CORE_DIR}/src/cft_capnp.c
    ${CORE_DIR}/src/cft_conn.c
    ${CORE_DIR}/src/cft_ingress.c
    ${CORE_DIR}/src/cft_poll.c
    ${CORE_DIR}/src/cft_proxy.c
    ${CORE_DIR}/src/cft_rpc.c
    ${CORE_DIR}/src/cft_token.c
    ${CORE_DIR}/src/cft_tunnel.c
    ${CORE_DIR}/src/cft_util.c
    transport_openssl.c
    transport_plain.c
)
target_include_directories(cf_tunnel PUBLIC ${CORE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${CORE_DIR}/src)
target_link_libraries(cf_tunnel PUBLIC ${NGHTTP2_LIB} OpenSSL::SSL OpenSSL::Crypto)
cft_warnings(cf_tunnel)
# Short keepalive timers, so tests of dead connections finish quickly
target_compile_definitions(cf_tunnel PRIVATE CFT_IDLE_PING_MS=1500 CFT_PING_TIMEOUT_MS=1500)

add_executable(cft_host main.c)
target_link_libraries(cft_host cf_tunnel)
cft_warnings(cft_host)

# RPC client test against mock_registration_server.py, which uses the
# reference Cap'n Proto implementation (pycapnp).
add_executable(rpc_test rpc_test.c)
target_include_directories(rpc_test PRIVATE ${CORE_DIR}/src)
target_link_libraries(rpc_test cf_tunnel)
cft_warnings(rpc_test)
