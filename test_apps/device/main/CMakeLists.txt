idf_component_register(SRCS "main.c" "../../common/test_handlers.c"
                       INCLUDE_DIRS "../../common"
                       PRIV_REQUIRES esp-cf-tunnel nvs-dotenv esp_eth esp_netif esp_event esp_timer pthread heap esp_psram)
# Trace WebSocket messages when debugging stalls: idf.py -DTRACE_WS=1 build
if(TRACE_WS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TH_TRACE_WS)
endif()
