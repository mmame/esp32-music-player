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
#define CMD_ACK             0xFF  /* Display → Host: ACK with optional touch   */

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

#ifdef __cplusplus
}
#endif
