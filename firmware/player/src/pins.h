/**
 * @file pins.h
 * @brief Central GPIO / hardware-assignment reference for the player firmware.
 *
 * All pin numbers are bare GPIO integers (ESP32-S3 numbering).
 * Adjust this file for your specific board layout; nothing else needs changing.
 */
#pragma once

/* ── SD card (SPI bus) ───────────────────────────────────────────────────── */
#define PIN_SD_CS       21
#define PIN_SPI_SCK     48
#define PIN_SPI_MOSI    38
#define PIN_SPI_MISO    47

/* ── I2S external DAC ────────────────────────────────────────────────────── */
#define MY_I2S_BCK      13  /* Bit clock                                      */
#define MY_I2S_WS       12  /* Word select / LRCK                             */
#define MY_I2S_DATA     14  /* Serial data / DIN                              */

/* ── UART1 link to Display-ESP ───────────────────────────────────────────── */
#define UM_UART_NUM     1   /* UART peripheral index                          */
#define UM_TX_PIN       40  /* Player TX  →  Display RX                       */
#define UM_RX_PIN       41  /* Player RX  ←  Display TX                       */

/* ── Rotary encoder ──────────────────────────────────────────────────────── */
#define ENC_PIN_A       5   /* Encoder channel A                              */
#define ENC_PIN_B       6   /* Encoder channel B                              */
#define ENC_PIN_BTN     3   /* Push-button, active-low (internal pull-up)     */

/* ── Potentiometers (ADC1 channel numbers = GPIO numbers on ESP32-S3) ────── */
#define POT_PIN_VOLUME  1   /* Master volume  – ADC1 / GPIO1                  */
#define POT_PIN_TEMPO   2   /* Playback speed – ADC1 / GPIO2                  */
