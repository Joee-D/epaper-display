#pragma once
#include <Arduino.h>
#include "config.h"

// ---------------------------------------------------------------------------
// Panel geometry — Waveshare 4.2" V2, SSD1683 controller, 400x300, b/w only.
// (This is NOT the same driver chip as the older 4.2" V1 board — V2 uses
// SSD1683, V1 uses UC8176/IL0398 — see display.cpp for the matching GxEPD2
// driver class.)
// ---------------------------------------------------------------------------
#define EPD_WIDTH  400
#define EPD_HEIGHT 300

// Expected raw image payload size the server must return:
// 1 bit per pixel, packed MSB-first, row-major, no padding between rows.
#define EPD_IMAGE_BYTES ((EPD_WIDTH * EPD_HEIGHT) / 8)

// ---------------------------------------------------------------------------
// Wiring — ESP32-WROOM-32E devkit, standard VSPI pins.
// ADJUST THESE if you wired it differently (e.g. if you're using
// Waveshare's dedicated "e-Paper ESP32 Driver Board" instead of a bare
// WROOM-32E devkit, its pinout is usually BUSY=25 RST=26 DC=27 CS=15
// SCK=13 MOSI=14 — check the silkscreen on your specific board).
// ---------------------------------------------------------------------------
#define PIN_EPD_SCK   13
#define PIN_EPD_MOSI  14
#define PIN_EPD_CS    15
#define PIN_EPD_DC    27
#define PIN_EPD_RST   26
#define PIN_EPD_BUSY  25

// Hold this pin LOW at boot to force the WiFi setup portal. GPIO0 is the
// BOOT/FLASH button on virtually every ESP32-WROOM-32E devkit.
#define PIN_CONFIG_BUTTON 0

// Initializes the panel using the configured orientation.
void displayInit(const DeviceConfig &cfg);

// Downloads the raw 1bpp bitmap from cfg.serverUrl and draws it full-screen.
// Returns true on success.
bool displayFetchAndShow(const DeviceConfig &cfg, const String &deviceId);

// Draws a simple centered two-line status/error message (used for setup
// mode, WiFi failures, fetch failures, etc.) so the screen is never blank.
void displayShowMessage(const String &line1, const String &line2);

// Puts the panel into deep sleep to avoid ghosting/damage while the MCU
// itself deep-sleeps.
void displayHibernate();
