// Shared layout + colors for every screen. The UI is laid out for landscape
// and scales to the board: 320x240 (2.4"/2.8"/3.2") or 480x320 (3.5").
#ifndef THEME_H
#define THEME_H

#include <TFT_eSPI.h>

// ---- Screen regions ----
#define UI_W            ((TFT_WIDTH) > (TFT_HEIGHT) ? (TFT_WIDTH) : (TFT_HEIGHT))
#define UI_H            ((TFT_WIDTH) > (TFT_HEIGHT) ? (TFT_HEIGHT) : (TFT_WIDTH))
#define UI_ROTATION     1       // landscape; 3 when "rotate 180" is set
#define UI_HEADER_H     18      // title + clock
#define UI_TAB_Y        18
#define UI_TAB_H        22      // tab labels + underline
#define UI_CONTENT_Y    41      // everything below the tab bar

// ---- Fonts (TFT_eSPI built-in) ----
#define UI_FONT_SMALL   1       // 6x8 px, labels
#define UI_FONT_BODY    2       // 16 px proportional, text
#define UI_FONT_BIG     4       // 26 px proportional, headline numbers

// ---- Colors (RGB565) ----
#define UI_BG           0x0000  // black
#define UI_CARD         0x10A2  // very dark grey card
#define UI_CARD_EDGE    0x2965  // subtle card outline
#define UI_ROW_ALT      0x0861  // alternating list rows
#define UI_TEXT         0xFFFF  // white
#define UI_DIM          0x9CF3  // light grey secondary text
#define UI_MUTED        0x528A  // grey for inactive things
#define UI_ACCENT       0x07FF  // cyan
#define UI_DOWN         0x3D7F  // download: sky blue
#define UI_UP           0xC2BF  // upload: violet
#define UI_GOOD         0x07E0  // green
#define UI_WARN         0xFEA0  // amber
#define UI_BAD          0xF904  // red
#define UI_ORANGE       0xFC40

#endif // THEME_H
