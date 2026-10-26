idf_component_register(
    SRCS
        "src/cft_capnp.c"
        "src/cft_conn.c"
        "src/cft_rpc.c"
        "src/cft_token.c"
        "src/cft_tunnel.c"
        "src/cft_util.c"
        "port/esp/cft_transport_esp.c"
        "port/esp/cft_tunnel_esp.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp-tls
    PRIV_REQUIRES esp_timer esp_hw_support
)
