/**
 * @file uart_master.h
 * @brief UART master protocol layer for the music-player (Host/Player side).
 *
 * Packet format – identical to the display firmware's uart_comm.h:
 *   [MAGIC: 8 bytes "ROGEL202"][CMD: 1 byte][LEN: 1 byte][PAYLOAD: LEN bytes][CHECKSUM: 1 byte]
 *
 * Checksum = XOR of CMD ^ LEN ^ payload[0] ^ ... ^ payload[LEN-1]
 *
 * Command direction reference:
 *   Host → Display : CMD_SET_STATE, CMD_SYNC, CMD_SONG_LIST,
 *                    CMD_ENCODER_MOVE, CMD_ENCODER_BTN, CMD_POTI_UPDATE
 *   Display → Host : CMD_PLAY_SONG, CMD_STOP_SONG, CMD_PAUSE, CMD_RESUME, CMD_ACK
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "pins.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Hardware configuration ───────────────────────────────────────────────── */
#define UM_BAUD_RATE        921600
#define UM_RX_BUF_SIZE      2048

/* ── Protocol constants ───────────────────────────────────────────────────── */

/* 8-byte magic "ROGEL202" */
static const uint8_t UM_MAGIC[8] = {
    0x52, 0x4F, 0x47, 0x45, 0x4C, 0x32, 0x30, 0x32
};

#define UM_MAX_PAYLOAD      128
#define UM_MAX_SONG_NAME    64

/* Command IDs – kept in sync with display firmware uart_comm.h */
#define CMD_SET_STATE       0x01  /* Host → Display: player state update       */
#define CMD_SYNC            0x02  /* Host → Display: heartbeat / ACK request   */
#define CMD_SONG_LIST       0x03  /* Host → Display: full playlist             */
#define CMD_ENCODER_MOVE    0x04  /* Host → Display: encoder delta             */
#define CMD_ENCODER_BTN     0x05  /* Host → Display: encoder button            */
#define CMD_PLAY_SONG       0x06  /* Display → Host: user chose a song (id)    */
#define CMD_POTI_UPDATE     0x07  /* Host → Display: live poti values          */
#define CMD_STOP_SONG       0x08  /* Display → Host: stop playback             */
#define CMD_PAUSE           0x09  /* Display → Host: pause playback            */
#define CMD_RESUME          0x0A  /* Display → Host: resume playback           */
#define CMD_DISPLAY_READY   0x0B  /* Display → Host: display reset, request resync */
#define CMD_SEEK            0x0C  /* Display → Host: seek to position (1-byte 0-100%) */
#define CMD_ST_BYPASS       0x0D  /* Display → Host: enable/disable SoundTouch bypass */
#define CMD_TEMPO_LOCK      0x0E  /* Display → Host: lock/unlock tempo at a fixed value */
#define CMD_WIFI_CTRL           0x0F  /* Display → Host: enable (1) / disable (0) WiFi AP  */
#define CMD_SONG_SETTINGS_REQ   0x10  /* Display → Host: request settings for a song        */
#define CMD_SONG_SETTINGS       0x11  /* Host → Display: current settings for a song        */
#define CMD_SET_SONG_SETTINGS   0x12  /* Display → Host: write new settings for a song      */
#define CMD_ACK                 0xFF  /* Display → Host: ACK with optional touch            */

/* ── Callbacks invoked from the UART receive task (Core 0) ───────────────── */

/** @brief Called when the display requests a specific song by 16-bit ID (1-based). */
typedef void (*um_on_play_song_cb_t)(uint16_t song_id);

/** @brief Called when the display sends CMD_STOP_SONG. */
typedef void (*um_on_stop_song_cb_t)(void);

/** @brief Called when the display sends CMD_PAUSE. */
typedef void (*um_on_pause_cb_t)(void);

/** @brief Called when the display sends CMD_RESUME. */
typedef void (*um_on_resume_cb_t)(void);

/** @brief Called when the display sends CMD_DISPLAY_READY (display was reset). */
typedef void (*um_on_display_ready_cb_t)(void);

/**
 * @brief Called when the display sends CMD_SEEK.
 * @param position_pct  Requested playback position 0–100 %.
 */
typedef void (*um_on_seek_cb_t)(uint8_t position_pct);

/**
 * @brief Called when the display sends CMD_ST_BYPASS.
 * @param bypass  true = bypass SoundTouch (lossless passthrough), false = normal.
 */
typedef void (*um_on_st_bypass_cb_t)(bool bypass);

/**
 * @brief Called when the display sends CMD_TEMPO_LOCK.
 * @param lock          true = lock tempo at @p locked_tempo, false = unlock.
 * @param locked_tempo  0–100 poti-scale value to lock at (ignored when lock == false).
 */
typedef void (*um_on_tempo_lock_cb_t)(bool lock, uint8_t locked_tempo);

/**
 * @brief Called when the display sends CMD_WIFI_CTRL.
 * @param enable  true = start WiFi AP + HTTP server, false = stop them.
 */
typedef void (*um_on_wifi_ctrl_cb_t)(bool enable);

/**
 * @brief Called when the display requests the settings for a song (CMD_SONG_SETTINGS_REQ).
 *
 * The handler should read the song's JSON sidecar and call
 * uart_master_send_song_settings() with the result.
 *
 * @param song_id  1-based song index as used by CMD_PLAY_SONG.
 */
typedef void (*um_on_song_settings_req_cb_t)(uint16_t song_id);

/**
 * @brief Called when the display writes new settings for a song (CMD_SET_SONG_SETTINGS).
 *
 * The handler should persist the settings to the song's JSON sidecar file.
 *
 * @param song_id          1-based song index.
 * @param flags            Bit-field: bit 0 = loop, bit 1 = fixed_speed_en.
 * @param fixed_speed_x100 Fixed speed multiplier × 100 (e.g. 100 = 1.0×, 120 = 1.2×).
 *                         Meaningful only when bit 1 of @p flags is set.
 */
typedef void (*um_on_set_song_settings_cb_t)(uint16_t song_id,
                                             uint8_t  flags,
                                             uint8_t  fixed_speed_x100);

/* ── Initialisation ───────────────────────────────────────────────────────── */

/**
 * @brief Initialise UART1 and start the receive task on Core 0.
 */
void uart_master_init(um_on_play_song_cb_t     on_play_song,
                      um_on_stop_song_cb_t     on_stop_song,
                      um_on_pause_cb_t         on_pause,
                      um_on_resume_cb_t        on_resume,
                      um_on_display_ready_cb_t on_display_ready);

/**
 * @brief Register a seek callback after init.
 *        Safe to call at any time; replaces the current callback.
 */
void uart_master_set_seek_callback(um_on_seek_cb_t on_seek);

/**
 * @brief Register a SoundTouch-bypass callback after init.
 *        Safe to call at any time; replaces the current callback.
 */
void uart_master_set_st_bypass_callback(um_on_st_bypass_cb_t on_st_bypass);

/**
 * @brief Register a tempo-lock callback after init.
 *        Safe to call at any time; replaces the current callback.
 */
void uart_master_set_tempo_lock_callback(um_on_tempo_lock_cb_t on_tempo_lock);

/**
 * @brief Register a WiFi-control callback after init.
 *        Safe to call at any time; replaces the current callback.
 */
void uart_master_set_wifi_ctrl_callback(um_on_wifi_ctrl_cb_t on_wifi_ctrl);

/**
 * @brief Register the callback for CMD_SONG_SETTINGS_REQ.
 */
void uart_master_set_song_settings_req_callback(um_on_song_settings_req_cb_t cb);

/**
 * @brief Register the callback for CMD_SET_SONG_SETTINGS.
 */
void uart_master_set_set_song_settings_callback(um_on_set_song_settings_cb_t cb);

/**
 * @brief Send CMD_SONG_SETTINGS to the display.
 *
 * @param song_id          1-based song index.
 * @param flags            Bit-field: bit 0 = loop, bit 1 = fixed_speed_en.
 * @param fixed_speed_x100 Fixed speed × 100 (e.g. 100 = 1.0×). 0 when bit 1 is clear.
 */
void uart_master_send_song_settings(uint16_t song_id,
                                    uint8_t  flags,
                                    uint8_t  fixed_speed_x100);

/* ── Outgoing packet helpers ──────────────────────────────────────────────── */

/**
 * @brief Send CMD_SONG_LIST to the display.
 *
 * Wire format per entry: [id_lo:u8][id_hi:u8][name:char…]['\0']
 * Terminator            : [0x00][0x00]
 */
void uart_master_send_song_list(const char names[][UM_MAX_SONG_NAME], uint8_t count);

/**
 * @brief Send CMD_SET_STATE to update the display with the current player state.
 *
 * Payload layout (little-endian):
 *   [0]       is_playing   : uint8_t   (1 = playing, 0 = stopped)
 *   [1]       volume       : uint8_t   (0–100)
 *   [2]       tempo        : uint8_t   (0–100, 50 = 1.0× speed)
 *   [3]       position_pct : uint8_t   (0–100, playback progress)
 *   [4..5]    duration_s   : uint16_t  (speed-adjusted song length in seconds)
 *   [6..N-1]  song_name    : char[]    (no null terminator; length = LEN - 6)
 */
void uart_master_send_state(const char *song_name,
                            uint8_t     is_playing,
                            uint8_t     volume,
                            uint8_t     tempo,
                            uint8_t     position_pct,
                            uint16_t    duration_s);

/**
 * @brief Send CMD_POTI_UPDATE so the display can refresh its visual bars.
 *
 * Payload layout (5 bytes):
 *   [0]  volume        : uint8_t  (0–100)
 *   [1]  tempo         : uint8_t  (0–100, 50 = 1.0×)
 *   [2]  expression    : uint8_t  (0–100, reserved)
 *   [3]  speed_min_x10 : uint8_t  (SPEED_MIN × 10, e.g. 5 for 0.5×)
 *   [4]  speed_max_x10 : uint8_t  (SPEED_MAX × 10, e.g. 20 for 2.0×)
 */
void uart_master_send_poti_update(uint8_t volume, uint8_t tempo, uint8_t expression,
                                  uint8_t speed_min_x10, uint8_t speed_max_x10);

/** @brief Send CMD_ENCODER_MOVE so the display can scroll its song list. */
void uart_master_send_encoder_move(int8_t delta);

/** @brief Send CMD_ENCODER_BTN so the display knows the encoder was pressed. */
void uart_master_send_encoder_btn(void);

/**
 * @brief Send CMD_SYNC and wait up to @p timeout_ms for CMD_ACK.
 * @return true if ACK was received within the timeout.
 */
bool uart_master_sync(uint32_t timeout_ms);

/* ── OTA support ──────────────────────────────────────────────────────────── */

/**
 * @brief Temporarily suspend UART1 communication for display OTA flashing.
 *
 * Sets a "paused" flag so pending send calls are dropped immediately, waits
 * for any in-flight transmission to complete, waits for the receive task to
 * become idle, then uninstalls the UART driver.
 *
 * Call uart_master_resume() to restore normal operation.
 * Must NOT be called before uart_master_init().
 */
void uart_master_pause(void);

/**
 * @brief Restore UART1 after a display OTA flash.
 *
 * Re-installs the UART1 driver with the original configuration and clears
 * the "paused" flag so the receive task and send functions resume normally.
 */
void uart_master_resume(void);

#ifdef __cplusplus
}
#endif
