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
#define MY_I2S_SCK      11  /* Master clock (MCLK) output to DAC                              */
#define MY_I2S_WS       12  /* Word select / LRCK                             */
#define MY_I2S_BCK      13  /* Bit clock                                      */
#define MY_I2S_DATA     14  /* Serial data / DIN                              */

/* ── UART1 link to Display-ESP ───────────────────────────────────────────── */
#define UM_UART_NUM     1   /* UART peripheral index                          */
//#define UM_TX_PIN       40  /* Player TX  →  Display RX                       */
//#define UM_RX_PIN       41  /* Player RX  ←  Display TX                       */
#define UM_TX_PIN       43  /* Player TX  →  Display RX                       */
#define UM_RX_PIN       44  /* Player RX  ←  Display TX                       */

/* ── Rotary encoder 1 (navigation) ──────────────────────────────────────── */
#define ENC_PIN_A       5   /* Encoder 1 channel A                            */
#define ENC_PIN_B       6   /* Encoder 1 channel B                            */

/* ── Rotary encoder 2 (organ-player speed wheel, 360 cycles/rev) ─────────── */
#define ENC2_PIN_A      7   /* Encoder 2 channel A                            */
#define ENC2_PIN_B      8   /* Encoder 2 channel B                            */

/* ── Resistor-ladder button array (ADC) ──────────────────────────────────── */
/* All buttons share one ADC pin.  R38 = 10 kΩ pull-up to 3V3.              */
/* Each button (CN8–CN17) connects the pin to GND through a series resistor. */
/* No internal pull-up – R38 is the only pull-up.                            */
#define BTN_ADC_PIN     3   /* ADC1 / GPIO3 – no internal pull-up             */
#define BTN_COUNT      10   /* Number of rungs in the ladder (CN8–CN17)       */

/*
 * ADC upper-boundary thresholds for each button rung (12-bit, 0–4095).
 * Midpoints between adjacent expected centre values; last entry = idle limit.
 *
 *   Rung  R_series (Ω)   ADC centre   Threshold (upper bound)
 *     0       0               0            130
 *     1     680             259            397
 *     2    1500             534            661
 *     3    2400             788            968
 *     4    3900            1147           1308
 *     5    5600            1468           1658
 *     6    8200            1848           2043
 *     7   12000            2237           2437
 *     8   18000            2637           2889
 *     9   33000            3140           3600   ← above = no button
 */
#define BTN_THRESHOLDS  { 130, 397, 661, 968, 1308, 1658, 2043, 2437, 2889, 3600 }

/* ── Potentiometers (ADC1 channel numbers = GPIO numbers on ESP32-S3) ────── */
#define POT_PIN_VOLUME  1   /* Master volume  – ADC1 / GPIO1                  */
#define POT_PIN_TEMPO   2   /* Playback speed – ADC1 / GPIO2                  */
