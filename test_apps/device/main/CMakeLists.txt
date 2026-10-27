idf_component_register(SRCS "main.c" "../../common/test_handlers.c"
                       INCLUDE_DIRS "../../common"
                       PRIV_REQUIRES esp-cf-tunnel nvs-dotenv esp_eth esp_netif esp_event esp_timer pthread heap)
