# From espressif/esp-board-manager esp_boards/esp_mosaico/usb_hs_console
# (Apache-2.0). The ESP-Mosaico's Type-C port is USB-OTG HS, which has no
# ROM console, so this TinyUSB CDC console carries logs and auto-download.

if(NOT IDF_TARGET STREQUAL "esp32s31")
    idf_component_register()
    return()
endif()

idf_component_register(
    SRCS "usb_console.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_timer esp_system
)

if(CONFIG_USB_HS_CONSOLE_USB_CDC_AUTO_INIT)
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=app_main"
        "-u __wrap_app_main"
    )
endif()
