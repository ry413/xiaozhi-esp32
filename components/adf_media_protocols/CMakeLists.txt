set(ADF_PATH "/Users/ry79/esp-adf")
if(DEFINED ENV{ADF_PATH})
    set(ADF_PATH "$ENV{ADF_PATH}")
endif()

set(ADF_LIBS_DIR "${ADF_PATH}/components/esp-adf-libs")

idf_component_register(
    SRCS
        "${ADF_LIBS_DIR}/media_lib_sal/media_lib_adapter.c"
        "${ADF_LIBS_DIR}/media_lib_sal/media_lib_common.c"
        "${ADF_LIBS_DIR}/media_lib_sal/media_lib_crypt.c"
        "${ADF_LIBS_DIR}/media_lib_sal/media_lib_netif.c"
        "${ADF_LIBS_DIR}/media_lib_sal/media_lib_os.c"
        "${ADF_LIBS_DIR}/media_lib_sal/media_lib_socket.c"
        "${ADF_LIBS_DIR}/media_lib_sal/media_lib_tls.c"
        "${ADF_LIBS_DIR}/media_lib_sal/mem_trace/media_lib_mem_trace.c"
        "${ADF_LIBS_DIR}/media_lib_sal/mem_trace/media_lib_mem_his.c"
        "${ADF_LIBS_DIR}/media_lib_sal/port/media_lib_crypt_default.c"
        "${ADF_LIBS_DIR}/media_lib_sal/port/media_lib_netif_default.c"
        "${ADF_LIBS_DIR}/media_lib_sal/port/media_lib_os_freertos.c"
        "${ADF_LIBS_DIR}/media_lib_sal/port/media_lib_socket_default.c"
        "${ADF_LIBS_DIR}/media_lib_sal/port/media_lib_tls_default.c"
    INCLUDE_DIRS
        "${ADF_LIBS_DIR}/esp_media_protocols/include"
        "${ADF_LIBS_DIR}/media_lib_sal/include"
        "${ADF_LIBS_DIR}/media_lib_sal/include/port"
    REQUIRES
        esp_http_client
        esp_http_server
        esp_netif
        esp-tls
        http_parser
        lwip
        mbedtls
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    CONFIG_MEDIA_PROTOCOL_LIB_ENABLE=1
    CONFIG_MEDIA_LIB_CRYPT_ENABLE=1
    CONFIG_MEDIA_LIB_SOCKET_ENABLE=1
    CONFIG_MEDIA_LIB_TLS_ENABLE=1
    CONFIG_MEDIA_LIB_NETIF_ENABLE=1
)

add_prebuilt_library(adf_esp_media_protocols
    "${ADF_LIBS_DIR}/esp_media_protocols/lib/${CONFIG_IDF_TARGET}/libesp_media_protocols.a"
    PRIV_REQUIRES adf_media_protocols
)

target_link_libraries(${COMPONENT_LIB} INTERFACE adf_esp_media_protocols)
