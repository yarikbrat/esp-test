idf_component_register(SRCS "main.c" "app.c" "kv.c" "flash_part.c" "bmp58x.c" "sonar.c"
                            "task_sensors.c" "task_logger.c" "task_bridge.c" "task_button.c"
                       INCLUDE_DIRS "."
                       REQUIRES core nvs_flash esp_partition esp_driver_uart esp_driver_i2c
                                esp_driver_gpio esp_timer
                       EMBED_FILES "../web/index.html")

# Стиснута gzip копія сторінки; віддається, коли браузер приймає gzip (менше байтів через UART).
set(PAGE_SRC ${COMPONENT_DIR}/../web/index.html)
set(PAGE_GZ ${CMAKE_CURRENT_BINARY_DIR}/index.html.gz)
add_custom_command(OUTPUT ${PAGE_GZ}
                   COMMAND sh -c "gzip -9 -n -c '${PAGE_SRC}' > '${PAGE_GZ}'"
                   DEPENDS ${PAGE_SRC} VERBATIM)
add_custom_target(page_gz DEPENDS ${PAGE_GZ})
add_dependencies(${COMPONENT_LIB} page_gz)
target_add_binary_data(${COMPONENT_LIB} ${PAGE_GZ} BINARY)
