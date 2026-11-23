set(WEB_PAGE "${CMAKE_CURRENT_LIST_DIR}/../../web/index.html")

idf_component_register(
    SRCS
        "main.c" "modem.c" "console.c" "pdu.c" "sms.c" "sms_assembly.c" "sms_seen.c" "sms_text.c"
        "pbbp2.c" "cmd.c" "map_store.c" "feishu.c" "feishu_proto.c" "cfg.c" "wifi.c"
        "ble.c" "health.c" "storage.c" "lan_web.c" "rpc.c"
        "wap_mms.c" "mms_policy.c" "mms_fetch.c" "mms_inbox.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_http_server esp_http_client espressif__esp_websocket_client
        esp_wifi esp_event esp_netif esp_timer esp_partition spi_flash
        esp_driver_gpio esp_driver_uart esp_driver_usb_serial_jtag
        bt nvs_flash fatfs wear_levelling json mbedtls esp_app_format lwip
    EMBED_TXTFILES "${WEB_PAGE}")
