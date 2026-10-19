#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "audio_element.h"
#include "fatfs_stream.h"      /* fatfs_stream_cfg_t and FATFS_STREAM_CFG_DEFAULT() are reused unchanged */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FAT file reader element: drop-in replacement for fatfs_stream_init() (reader only) that reads through
 *        an internal DMA-capable buffer, which is several times faster on the ESP32-S3 than reading into PSRAM.
 *        It can also crossfade from the file it is streaming into another file (see below).
 *
 * @param config  Same configuration as fatfs_stream; type must be AUDIO_STREAM_READER.  A read request is split
 *                into chunks of at most min(buf_sz, 32 KB) taken from the internal buffer (16 KB is a good size).
 * @return The element handle, or NULL on failure.
 */
audio_element_handle_t fatfs_dma_stream_init(fatfs_stream_cfg_t *config);

/**
 * @brief When on, the data-size field of the stream's WAV header (read from the file at its start) is replaced
 *        by a huge value.  Needed before a crossfade: the decoder takes the stream length from the first header it
 *        sees, but after a crossfade the stream contains another song.  Takes effect the next time a file is
 *        opened by the element.
 */
void fatfs_dma_stream_set_unbounded_header(audio_element_handle_t el, bool on);

/**
 * @brief Stop streaming at file offset @p end_off (end of the PCM data; 0 = no limit).  With the unbounded
 *        header the decoder no longer knows where the audio ends, so this keeps chunks after the data out.
 */
void fatfs_dma_stream_set_limit(audio_element_handle_t el, uint32_t end_off);

/**
 * @brief Crossfade from the file being streamed into @p new_path.
 *
 * The reader keeps reading the outgoing file and starts the incoming file, then outputs the equal-power mix of
 * both for @p fade_bytes of output, after which only the incoming file continues.  Both files must be 16-bit
 * PCM WAV with the same format; offsets are positions in the files (canonical 44-byte header).  The pipeline
 * keeps running; the transition becomes audible when the data already buffered downstream has been played.
 *
 * @param old_end_off    file offset where the outgoing file's PCM ends
 * @param new_start_off  file offset where the incoming file's PCM starts (44)
 * @param new_end_off    file offset where the incoming file's PCM ends
 * @param old_mode       downmix mode of the outgoing song (0 mix, 1 CH1, 2 CH2)
 * @param new_mode       downmix mode of the incoming song
 * @param stereo         true: downmix each song with its own mode (to L = R) before mixing them;
 *                       the consumer must treat the stream as already downmixed during the fade
 * @return ESP_OK if the request was accepted; ESP_ERR_INVALID_STATE if the reader is not streaming, is already
 *         crossfading, or the outgoing file is (almost) finished; the caller should then switch songs normally.
 */
esp_err_t fatfs_dma_stream_crossfade(audio_element_handle_t el, const char *new_path, uint32_t fade_bytes,
                                     uint32_t old_end_off, uint32_t new_start_off, uint32_t new_end_off,
                                     uint8_t old_mode, uint8_t new_mode, bool stereo);

/**
 * @brief Reverse playback: stream the PCM data backwards, starting just below the element's byte position
 *        (set with audio_element_setinfo before the pipeline starts) down to the start of the PCM (offset 44),
 *        where the stream ends.  Every block is read from lower in the file and its sample frames are reversed.
 *        Takes effect the next time the file is opened.  Not combined with a crossfade.
 *
 * @param frame_bytes  bytes per sample frame (channels * bytes per sample), e.g. 4 for 16-bit stereo
 */
void fatfs_dma_stream_set_reverse(audio_element_handle_t el, bool on, uint32_t frame_bytes);

/** True while a requested crossfade has not finished yet. */
bool fatfs_dma_stream_crossfade_busy(audio_element_handle_t el);

#ifdef __cplusplus
}
#endif
