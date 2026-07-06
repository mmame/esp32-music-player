/**
 * @file uart_comm.h
 * @brief UART communication layer for the music-player display firmware.
 *
 * Packet format (all fields little-endian where multi-byte):
 *   [MAGIC: 8 bytes "ROGEL202"][CMD: 1 byte][LEN: 1 byte][PAYLOAD: LEN bytes][CHECKSUM: 1 byte]
 *
 * Checksum = XOR of CMD ^ LEN ^ payload[0] ^ ... ^ payload[LEN-1]
 *
 * Poll-response protocol (Display → Host direction)
 * -------------------------------------------------
 * The display NEVER sends unsolicited packets to the player.  Instead, all
 * Display→Host commands are queued internally and flushed in a CMD_ACK
 * response each time CMD_SET_STATE or CMD_SYNC is received from the player.
 *
 * Extended CMD_ACK payload layout:
 *   [0]     touch_active : uint8_t
 *   [1..2]  touch_x      : int16_t  LE
 *   [3..4]  touch_y      : int16_t  LE
 *   [5]     cmd_count    : uint8_t  (number of queued sub-commands)
 *   For each sub-command:
 *     [n]   cmd_id       : uint8_t
 *     [n+1] param_len    : uint8_t
 *     [n+2..n+1+param_len] params
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Hardware configuration ---------- */
#define UART_COMM_PORT          UART_NUM_1
#define UART_COMM_TX_PIN        GPIO_NUM_43
#define UART_COMM_RX_PIN        GPIO_NUM_44
#define UART_COMM_BAUD_RATE     921600
#define UART_COMM_RX_BUF_SIZE   (2048)

/* ---------- Protocol constants ---------- */

/* 64-bit magic header "ROGEL202" (0x524F47454C323032), transmitted MSB-first */
static const uint8_t UART_MAGIC_BYTES[8] = {
    0x52, 0x4F, 0x47, 0x45, 0x4C, 0x32, 0x30, 0x32
};

#define MAX_PAYLOAD_LEN         128
#define MAX_SONG_NAME_LEN       64

/* Accumulation buffer for multi-packet CMD_SONG_LIST merging.
 * Sized for SONGLIST_MAX_SONGS (64) × max entry = 64 × (2+64+1) + 3 terminator. */
#define SONGLIST_BUF_SIZE       4352

/* Command IDs */
#define CMD_SET_STATE   0x01    /* Host → Display: update player state          */
#define CMD_SYNC        0x02    /* Host → Display: request sync / ACK           */
#define CMD_SONG_LIST   0x03    /* Host → Display: batch song-list update       */
#define CMD_ENCODER_MOVE 0x04   /* Host → Display: encoder rotation delta       */
#define CMD_ENCODER_BTN 0x05    /* Host → Display: encoder button pressed       */
#define CMD_PLAY_SONG   0x06    /* Display → Host: user selected a song         */
#define CMD_POTI_UPDATE 0x07    /* Host → Display: live poti values             */
#define CMD_STOP_SONG   0x08    /* Display → Host: stop playback                */
#define CMD_PAUSE       0x09    /* Display → Host: pause playback               */
#define CMD_RESUME      0x0A    /* Display → Host: resume playback              */
#define CMD_DISPLAY_READY 0x0B  /* Display → Host: display reset, request resync */
#define CMD_SEEK        0x0C    /* Display -> Host: seek to position (1-byte pct) */
#define CMD_ST_BYPASS   0x0D    /* Display -> Host: enable/disable SoundTouch bypass */
#define CMD_TEMPO_LOCK  0x0E    /* Display -> Host: lock/unlock tempo at a fixed value */
#define CMD_WIFI_CTRL           0x0F  /* Display -> Host: enable (1) / disable (0) WiFi AP  */
#define CMD_SONG_SETTINGS_REQ   0x10  /* Display -> Host: request settings for a song       */
#define CMD_SONG_SETTINGS       0x11  /* Host -> Display: current settings for a song       */
#define CMD_SET_SONG_SETTINGS   0x12  /* Display -> Host: write new settings for a song     */
#define CMD_ACK                 0xFF  /* Display -> Host: sync acknowledgement              */

/* ---------- Global system state ---------- */
typedef struct {
    char     song_name[MAX_SONG_NAME_LEN];  /* null-terminated UTF-8 */
    uint8_t  is_playing;                    /* 1 = playing, 0 = paused/stopped */
    uint8_t  volume;                        /* 0–100 */
    uint8_t  tempo;                         /* 0–100 (50 = 1.0× speed) */
    uint8_t  position_pct;                  /* playback progress 0–100 % */
    uint16_t duration_s;                    /* speed-adjusted total length in seconds */
} music_player_state_t;

/** Shared player state – read by the UI, written by the UART task. */
extern music_player_state_t g_player_state;

/* ---------- Public API ---------- */

/**
 * @brief Initialise UART1 at 921600 baud and start the UART task on Core 0.
 *        Must be called after the LVGL / BSP initialisation is complete.
 */
void uart_comm_init(void);

/**
 * @brief Send CMD_ST_BYPASS to the player.
 *
 * @param bypass  true  = bypass SoundTouch (lossless passthrough output),
 *                false = normal time-stretch mode.
 */
void uart_comm_send_st_bypass(bool bypass);

/**
 * @brief Send CMD_TEMPO_LOCK to the player.
 *
 * @param lock          true  = lock tempo at @p locked_tempo (ignore poti),
 *                      false = unlock (resume poti-controlled tempo).
 * @param locked_tempo  0–100 poti-scale value to lock at.  Ignored when
 *                      @p lock is false but should still be a valid value.
 */
void uart_comm_send_tempo_lock(bool lock, uint8_t locked_tempo);

/**
 * @brief Send CMD_WIFI_CTRL to the player.
 *
 * @param enable  true  = start WiFi AP + HTTP server on the player,
 *                false = stop them.
 */
void uart_comm_send_wifi_ctrl(bool enable);

/**
 * @brief Send CMD_SONG_SETTINGS_REQ to the player to request the current
 *        settings for a specific song.  The player will reply with
 *        CMD_SONG_SETTINGS which is delivered via ui_songlist_song_settings_async().
 *
 * @param song_id  1-based song index.
 */
void uart_comm_send_song_settings_req(uint16_t song_id);

/**
 * @brief Send CMD_SET_SONG_SETTINGS to write new settings for a song to the
 *        player's SD card.
 *
 * @param song_id          1-based song index.
 * @param flags            Bit-field: bit 0 = loop, bit 1 = fixed_speed_en.
 * @param fixed_speed_x100 Fixed speed × 100 (e.g. 100 = 1.0×).
 *                         Only meaningful when bit 1 of @p flags is set.
 */
void uart_comm_send_set_song_settings(uint16_t song_id,
                                      uint8_t  flags,
                                      uint8_t  fixed_speed_x100);

/**
 * @brief Enqueue CMD_PLAY_SONG to be sent on the next poll response.
 * @param song_id  1-based song index.
 */
void uart_comm_send_play_song(uint16_t song_id);

/**
 * @brief Enqueue CMD_STOP_SONG to be sent on the next poll response.
 */
void uart_comm_send_stop(void);

/**
 * @brief Enqueue CMD_PAUSE to be sent on the next poll response.
 */
void uart_comm_send_pause(void);

/**
 * @brief Enqueue CMD_RESUME to be sent on the next poll response.
 */
void uart_comm_send_resume(void);

/**
 * @brief Enqueue CMD_SEEK to be sent on the next poll response.
 * @param pct  Seek target 0–100 %.
 */
void uart_comm_send_seek(uint8_t pct);

/**
 * @brief Update the touch co-ordinates used in ACK responses.
 *
 * Call this from the LVGL indev read callback each time touch data is
 * obtained, so the UART task always has up-to-date touch information
 * without needing to acquire the LVGL lock.
 *
 * @param active  true if the touchscreen is currently pressed.
 * @param x       Touch X co-ordinate (ignored when active == false).
 * @param y       Touch Y co-ordinate (ignored when active == false).
 */
void uart_comm_update_touch(bool active, int16_t x, int16_t y);

#ifdef __cplusplus
}
#endif
