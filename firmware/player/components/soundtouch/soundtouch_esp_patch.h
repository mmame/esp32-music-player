/**
 * @file soundtouch_esp_patch.h
 *
 * Force-included before every SoundTouch translation unit (via
 * -include in CMakeLists.txt).
 *
 * Problem: STTypes.h defines ST_THROW_RT_ERROR as a C++ throw expression,
 * but ESP-IDF compiles with -fno-exceptions, so any throw is an error.
 *
 * Fix: include STTypes.h here first (its include guard will prevent a
 * second inclusion later), then redefine the macro to call abort()
 * instead of throw.  The result is identical behaviour for a correctly
 * parameterised SoundTouch instance, and a hard reset instead of an
 * uncatchable exception if something goes wrong.
 */
#pragma once

/* Pull in STTypes.h now so its include guard fires.  Subsequent includes
 * of STTypes.h (from AAFilter.h, FIFOSamplePipe.h, etc.) will be no-ops. */
#include "STTypes.h"

/* Replace the throw-based error macro with abort(). */
#undef  ST_THROW_RT_ERROR
#define ST_THROW_RT_ERROR(x)  do { abort(); } while (0)
