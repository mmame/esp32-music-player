# PlatformIO with ESP-IDF framework treats src/ as the "main" IDF component.
# All source files are listed in SRCS.  ESP-ADF component REQUIRES are only
# appended when ADF_PATH is available so the CMake/VS-Code build (no ADF)
# still succeeds.

# ── Core ESP-IDF requirements (always available) ──────────────────────────
set(MAIN_REQUIRES
    nvs_flash   # NVS (required at boot)
    driver      # GPIO, SPI, UART, I2S, PCNT, SDMMC – umbrella component
    fatfs       # FAT filesystem + esp_vfs_fat_sdspi_mount
    esp_adc     # ADC oneshot driver (potentiometers)
    esp_timer   # esp_timer_get_time() for wall-clock tracking
)

# ── ESP-ADF requirements (only when ADF_PATH is set) ─────────────────────
if(DEFINED ENV{ADF_PATH})
    list(APPEND MAIN_REQUIRES
        # Pipeline infrastructure (provides audio_element, audio_event_iface, audio_common.h)
        audio_pipeline
        audio_sal
        # All streams: fatfs_stream, i2s_stream, raw_stream, …
        audio_stream
        # Codecs + processing: wav_decoder, audio_alc, …
        esp-adf-libs
        # SoundTouch time-stretching element (replaces audio_sonic)
        soundtouch
    )
endif()

idf_component_register(
    SRCS
        "main.cpp"
        "encoder.cpp"
        "potis.cpp"
        "uart_master.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        ${MAIN_REQUIRES}
)

# Expose HAVE_ADF compile-time flag so main.cpp can conditionally compile
# the ADF pipeline code.  Must come AFTER idf_component_register() so that
# COMPONENT_TARGET exists and the call is not executed in script mode.
if(DEFINED ENV{ADF_PATH})
    target_compile_definitions(${COMPONENT_TARGET} PRIVATE HAVE_ADF=1)
endif()
