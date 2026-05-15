cmake_minimum_required(VERSION 3.16)

# ── Target chip – must be set before including project.cmake ─────────────────
# The VS Code ESP-IDF extension sets $ENV{IDF_TARGET}=esp32 from its own internal
# state (ignoring idf.idfTarget in settings.json).  Override the env variable
# here so __target_init() in targets.cmake picks up esp32s3 first.
set(ENV{IDF_TARGET} "esp32s3")

# ── ESP-ADF integration ──────────────────────────────────────────────────────
# ADF_PATH is injected by scripts/setup_adf.py (a PlatformIO pre-build script)
# before CMake runs. It can also be set manually in the environment.
# Fallback: auto-detect esp-adf/ in the project root (works with IDF extension).
if(DEFINED ENV{ADF_PATH})
    set(ADF_PATH $ENV{ADF_PATH})
    message(STATUS "[ESP-ADF] ADF_PATH from environment = ${ADF_PATH}")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/esp-adf/CMakeLists.txt")
    set(ADF_PATH "${CMAKE_CURRENT_LIST_DIR}/esp-adf")
    set(ENV{ADF_PATH} "${ADF_PATH}")   # ADF's own CMakeLists.txt reads $ENV{ADF_PATH}
    message(STATUS "[ESP-ADF] Auto-detected at ${ADF_PATH}")
endif()

if(ADF_PATH)
    # Let ESP-ADF register its own component directories
    include("${ADF_PATH}/CMakeLists.txt")
else()
    message(WARNING "[ESP-ADF] ADF_PATH is not set – ESP-ADF will not be available. "
                    "Run a PlatformIO build to trigger the setup script, or set ADF_PATH manually.")
endif()

# ── App sources (src/ maps to the ESP-IDF "main" component) ─────────────────
# ESP-IDF only auto-discovers ${CMAKE_SOURCE_DIR}/main/ by default.
# We add src/ explicitly so idf_component_register() there is processed.
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/src")

# ── PlatformIO-managed library dependencies ──────────────────────────────────
# If lib_deps are used in platformio.ini, PlatformIO installs them under
# .pio/libdeps/<env>. Adding that directory makes them visible to ESP-IDF CMake.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/.pio/libdeps")
    file(GLOB _pio_libdeps "${CMAKE_CURRENT_LIST_DIR}/.pio/libdeps/*")
    list(APPEND EXTRA_COMPONENT_DIRS ${_pio_libdeps})
endif()

# ── ESP-IDF project ──────────────────────────────────────────────────────────
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(musicplayer)
