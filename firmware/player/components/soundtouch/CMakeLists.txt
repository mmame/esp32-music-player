cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# Download SoundTouch 2.3.3 on first configure using git clone.
#
# FetchContent cannot be used here because IDF evaluates component
# CMakeLists files in CMake script mode (-P) during its dependency-scan
# pass, and FetchContent internally calls define_property which is not
# available in script mode.
#
# execute_process IS available in script mode, so we use a plain git clone
# instead.  The result is cached inside the component directory so internet
# access is only needed once.
# ---------------------------------------------------------------------------

set(ST_CACHE_DIR "${CMAKE_CURRENT_LIST_DIR}/soundtouch_src")

if(NOT EXISTS "${ST_CACHE_DIR}/include/SoundTouch.h")
    message(STATUS "[soundtouch] Cloning SoundTouch 2.3.3 from Codeberg ...")
    execute_process(
        COMMAND git clone
            --depth 1
            --branch 2.3.3
            https://codeberg.org/soundtouch/soundtouch.git
            "${ST_CACHE_DIR}"
        RESULT_VARIABLE _git_result
        OUTPUT_VARIABLE _git_stdout
        ERROR_VARIABLE  _git_stderr
    )
    if(NOT _git_result EQUAL 0)
        message(FATAL_ERROR "[soundtouch] git clone failed (exit ${_git_result}):\n${_git_stderr}")
    endif()
    message(STATUS "[soundtouch] SoundTouch ready at ${ST_CACHE_DIR}")
endif()

set(ST_SRC "${ST_CACHE_DIR}/source/SoundTouch")

# Core library files.  cpu_detect_x86.cpp is intentionally excluded –
# it contains x86 inline assembly that does not compile on Xtensa.
set(ST_SRCS
    "${ST_SRC}/AAFilter.cpp"
    "${ST_SRC}/FIFOSampleBuffer.cpp"
    "${ST_SRC}/FIRFilter.cpp"
    "${ST_SRC}/InterpolateLinear.cpp"
    "${ST_SRC}/InterpolateCubic.cpp"
    "${ST_SRC}/InterpolateShannon.cpp"
    "${ST_SRC}/RateTransposer.cpp"
    "${ST_SRC}/SoundTouch.cpp"
    "${ST_SRC}/TDStretch.cpp"
    "${ST_SRC}/PeakFinder.cpp"
    "${ST_SRC}/BPMDetect.cpp"
)

idf_component_register(
    SRCS
        "soundtouch_el.cpp"
        "cpu_detect_stub.cpp"
        ${ST_SRCS}
    INCLUDE_DIRS
        "."
        "${ST_CACHE_DIR}/include"
        "${ST_SRC}"
    REQUIRES
        audio_pipeline
        audio_sal
        log
)

# SIMD is gated on __SSE__ / __ARM_NEON__ etc. – not defined for Xtensa,
# so it is already dead code.  The define below just silences any residual
# SoundTouch #pragma that checks for the macro explicitly.
target_compile_definitions(${COMPONENT_TARGET} PRIVATE
    SOUNDTOUCH_ALLOW_NONEXACT_SIMD_OPTIMIZATION=0
    SOUNDTOUCH_INTEGER_SAMPLES=1
)

# SoundTouch uses patterns that trigger harmless warnings.
# -Wno-unknown-pragmas   : silences #pragma omp parallel (no OpenMP on Xtensa)
# -include patch header  : replaces ST_THROW_RT_ERROR throw→abort() because
#                          ESP-IDF builds with -fno-exceptions
target_compile_options(${COMPONENT_TARGET} PRIVATE
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-type-limits
    -Wno-shadow
    -Wno-unknown-pragmas
    -Wno-error=unknown-pragmas
    -Wno-missing-field-initializers
    "-include${CMAKE_CURRENT_LIST_DIR}/soundtouch_esp_patch.h"
)
