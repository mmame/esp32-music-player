/**
 * @file uart_comm.h
 * @brief UART communication layer for the music-player display firmware.
 *
 * Packet format (all fields little-endian where multi-byte):
 *   [MAGIC: 8 bytes "ROGEL202"][CMD: 1 byte][LEN: 1 byte][PAYLOAD: LEN bytes][CHECKSUM: 1 byte]
 *
 * Checksum = XOR of CMD ^ LEN ^ payload[0] ^ ... ^ payload[LEN-1]
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Hardware configuration ---------- */
#define UART_COMM_PORT          UART_NUM_1
#define UART_COMM_TX_PIN        GPIO_NUM_17
#define UART_COMM_RX_PIN        GPIO_NUM_18
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
#define CMD_ACK         0xFF    /* Display → Host: sync acknowledgement         */

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
