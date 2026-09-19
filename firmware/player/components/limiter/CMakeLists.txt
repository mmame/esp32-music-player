cmake_minimum_required(VERSION 3.16)

# Peak limiter / static gain element built on ESP-ADF's esp_audio_effects DRC.
#
# esp_audio_effects lives one directory level too deep for ESP-ADF to register it
# as a component (esp-adf-libs/esp_audio_effects), so its headers and prebuilt
# library are wired up here directly.
if(DEFINED ENV{ADF_PATH})
    set(_ae_dir "$ENV{ADF_PATH}/components/esp-adf-libs/esp_audio_effects")
else()
    set(_ae_dir "${CMAKE_CURRENT_LIST_DIR}/../../esp-adf/components/esp-adf-libs/esp_audio_effects")
endif()

idf_component_register(
    SRCS
        "limiter_el.c"
    INCLUDE_DIRS
        "."
        "${_ae_dir}/include"
    REQUIRES
        audio_pipeline
        audio_sal
        log
)

target_link_libraries(${COMPONENT_LIB} PUBLIC
    "${_ae_dir}/lib/${CONFIG_IDF_TARGET}/libesp_audio_effects.a")
