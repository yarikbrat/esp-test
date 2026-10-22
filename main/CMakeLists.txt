idf_component_register(SRCS "main.c" INCLUDE_DIRS "." REQUIRES core
                       EMBED_FILES "../web/index.html")

# Gzipped copy of the page, served when the browser accepts gzip (fewer bytes over the UART).
set(PAGE_SRC ${COMPONENT_DIR}/../web/index.html)
set(PAGE_GZ ${CMAKE_CURRENT_BINARY_DIR}/index.html.gz)
add_custom_command(OUTPUT ${PAGE_GZ}
                   COMMAND sh -c "gzip -9 -n -c '${PAGE_SRC}' > '${PAGE_GZ}'"
                   DEPENDS ${PAGE_SRC} VERBATIM)
add_custom_target(page_gz DEPENDS ${PAGE_GZ})
add_dependencies(${COMPONENT_LIB} page_gz)
target_add_binary_data(${COMPONENT_LIB} ${PAGE_GZ} BINARY)
