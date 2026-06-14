idf_component_register(
    SRCS
        "main.c"
        "st7789.c"
        "gui.c"
        "es8311.c"
        "m5pm1.cpp"
        "audio.c"
        "watering.c"
    INCLUDE_DIRS "." "voice"
    REQUIRES
        driver
        esp_adc
        esp_driver_i2c
        esp_timer
        freertos
        log
        m5stack__m5pm1
        spi_flash
        espressif__esp_codec_dev
)

target_link_libraries(${COMPONENT_LIB} PRIVATE m)
