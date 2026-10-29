# Hauptkomponente. "main" hängt in ESP-IDF automatisch von allen Komponenten ab;
# die Registry-Komponenten (mdns, littlefs, led_strip, cjson) kommen über idf_component.yml.

# Web-UI als gzip-Byte-Arrays in web_assets_gen.h einbetten.
# Läuft bei jedem CMake-Configure; PlatformIO ruft das Skript zusätzlich als pre-Skript.
set(WEB_FILES
    ${CMAKE_CURRENT_LIST_DIR}/web/index.html
    ${CMAKE_CURRENT_LIST_DIR}/web/app.js
    ${CMAKE_CURRENT_LIST_DIR}/web/style.css
    ${CMAKE_CURRENT_LIST_DIR}/web/qrcode.js
)
set(WEB_GEN_H ${CMAKE_CURRENT_LIST_DIR}/web_assets_gen.h)
idf_build_get_property(python PYTHON)
if(NOT python)
    set(python python)
endif()
execute_process(
    COMMAND ${python} ${CMAKE_CURRENT_LIST_DIR}/../tools/embed_web_assets.py ${WEB_GEN_H} ${WEB_FILES}
    RESULT_VARIABLE web_embed_rc
)
if(NOT web_embed_rc EQUAL 0)
    message(FATAL_ERROR "tools/embed_web_assets.py fehlgeschlagen (rc=${web_embed_rc})")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${WEB_FILES})

idf_component_register(
    SRCS
        "main.c"
        "settings.c"
        "event_log.c"
        "storage.c"
        "status_led.c"
        "time_sync.c"
        "net_eth.c"
        "cert_store.c"
        "auth.c"
        "web_server.c"
        "codes_db.c"
        "door_relay.c"
        "keypad_rs232.c"
        "access.c"
        "api_tokens.c"
        "ota_update.c"
    INCLUDE_DIRS
        "."
)
