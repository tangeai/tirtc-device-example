idf_component_register(
    SRCS "src/at_transport.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_uart
        esp_driver_usb_serial_jtag
)
