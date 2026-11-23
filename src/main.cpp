// CYD UniFi Monitor
// A live UniFi network dashboard for ESP32 "Cheap Yellow Display" boards.
#include <Arduino.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <time.h>
#include "config.h"
#include "theme.h"
#include "settings.h"
#include "unifi_api.h"
#include "ui_pages.h"

#ifdef TOUCH_XPT_OWN_SPI
#include <XPT2046_Touchscreen.h>
#endif

#if defined(USE_CREDENTIALS_H) && __has_include("credentials.h")
#include "credentials.h"
#define HAVE_BUILTIN_CREDENTIALS 1
#endif

TFT_eSPI tft = TFT_eSPI();
UniFiAPI unifiAPI;
Settings settings;
static int rotation = UI_ROTATION;

// ====================================================================== touch
//
// Two kinds of CYD touch wiring:
//  * TOUCH_XPT_OWN_SPI: XPT2046 on its own pins (2.8" CYDs) -> driven on VSPI
//  * TOUCH_CS:          XPT2046 shares the display bus (2.4"/3.2"/3.5") ->
//                       TFT_eSPI's touch support + a one-time on-screen calibration

#ifdef TOUCH_XPT_OWN_SPI
#define TOUCH_PIN_CS   33
#define TOUCH_PIN_IRQ  36
#define TOUCH_PIN_MOSI 32
#define TOUCH_PIN_MISO 39
#define TOUCH_PIN_CLK  25
#define TOUCH_RAW_X_MIN 200
#define TOUCH_RAW_X_MAX 3700
#define TOUCH_RAW_Y_MIN 240
#define TOUCH_RAW_Y_MAX 3800
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen touchscreen(TOUCH_PIN_CS, TOUCH_PIN_IRQ);
#endif

static void touchBegin() {
#ifdef TOUCH_XPT_OWN_SPI
    touchSPI.begin(TOUCH_PIN_CLK, TOUCH_PIN_MISO, TOUCH_PIN_MOSI, TOUCH_PIN_CS);
    touchscreen.begin(touchSPI);
    touchscreen.setRotation(rotation);
#elif defined(TOUCH_CS)
    uint16_t cal[5];
    if (!settingsLoadTouchCal(cal)) {
        tft.fillScreen(UI_BG);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(UI_TEXT);
        tft.drawString("Touch calibration", UI_W / 2, UI_H / 2 - 12, UI_FONT_BODY);
        tft.setTextColor(UI_DIM);
        tft.drawString("Tap each corner arrow as it appears", UI_W / 2, UI_H / 2 + 10, UI_FONT_BODY);
        tft.setTextDatum(TL_DATUM);
        delay(1500);
        tft.calibrateTouch(cal, UI_ACCENT, UI_BG, 15);
        settingsSaveTouchCal(cal);
    }
    tft.setTouch(cal);
#endif
}

// Is a finger on the screen right now (used for "hold to open setup")
static bool touchHeld() {
#ifdef TOUCH_XPT_OWN_SPI
    return touchscreen.touched();
#elif defined(TOUCH_CS)
    return tft.getTouchRawZ() > 400;
#else
    return false;
#endif
}

static bool touchRead(int& x, int& y) {
#ifdef TOUCH_XPT_OWN_SPI
    if (!(touchscreen.tirqTouched() && touchscreen.touched())) return false;
    TS_Point p = touchscreen.getPoint();
    if (p.z < 200) return false;
    x = constrain(map(p.x, TOUCH_RAW_X_MIN, TOUCH_RAW_X_MAX, 0, UI_W - 1), 0, UI_W - 1);
    y = constrain(map(p.y, TOUCH_RAW_Y_MIN, TOUCH_RAW_Y_MAX, 0, UI_H - 1), 0, UI_H - 1);
    return true;
#elif defined(TOUCH_CS)
    uint16_t tx, ty;
    if (!tft.getTouch(&tx, &ty, 300)) return false;
    x = tx;
    y = ty;
    return true;
#else
    return false;
#endif
}

// ===================================================================== layout

#define SCREEN_WIDTH     UI_W
#define SCREEN_HEIGHT    UI_H
#define CONTENT_START_Y  UI_CONTENT_Y
#define STATS_HEIGHT     (UI_H >= 300 ? 84 : 67)
#define SPEED_BOX_WIDTH  (UI_W >= 480 ? 130 : 92)
#define GRAPH_X          4
#define GRAPH_Y          (CONTENT_START_Y + 4)
#define GRAPH_HEIGHT     (UI_H - GRAPH_Y - 8 - STATS_HEIGHT)              // 120 on 320x240
#define GRAPH_WIDTH      (UI_W - 12 - SPEED_BOX_WIDTH)                    // 216 on 320x240
#define SPEED_BOX_X      (GRAPH_X + GRAPH_WIDTH + 4)
#define SPEED_BOX_Y      GRAPH_Y
#define SPEED_BOX_HEIGHT GRAPH_HEIGHT
#define STATS_Y          (GRAPH_Y + GRAPH_HEIGHT + 4)
#define STAT_BOX_WIDTH   ((UI_W - 20) / 4)                                // 75 on 320 wide
#define STAT_X(i)        (4 + (i) * (STAT_BOX_WIDTH + 4))

// ====================================================================== state

NetworkStats currentStats;
float smoothDownload = 0, smoothUpload = 0;
float targetDownload = 0, targetUpload = 0;

#define GRAPH_POINTS 60
float downloadHistory[GRAPH_POINTS] = {0};
float uploadHistory[GRAPH_POINTS] = {0};

unsigned long lastApiUpdate = 0, lastExperienceUpdate = 0, lastLoginAttempt = 0;
unsigned long lastGraphScroll = 0, lastDisplayUpdate = 0, lastStatsRedraw = 0;
const unsigned long GRAPH_SCROLL_INTERVAL = 1000;
const unsigned long DISPLAY_UPDATE_INTERVAL = 500;
const unsigned long STATS_REDRAW_INTERVAL = 2000;

float prevDrawnDownload = -1, prevDrawnUpload = -1;

int currentTab = TAB_OVERVIEW;
int prevTab = 0;
bool tabNeedsRedraw = true;

unsigned long lastTouchTime = 0;
const unsigned long TOUCH_DEBOUNCE_MS = 300;
bool touchPressed = false;

// Off-screen buffer for the throughput graph (only allocated while Overview is shown)
static TFT_eSprite graphSprite = TFT_eSprite(&tft);
static bool graphSpriteOk = false;
static void releaseGraphSprite() {
    if (graphSpriteOk) {
        graphSprite.deleteSprite();
        graphSpriteOk = false;
    }
}

void drawHeader();
void drawTabBar();
void drawStaticElements();
void updateSpeedDisplay();
void updateGraphDisplay();
void scrollGraphData();
void updateStatsBar();
void handleTouch();

// ============================================================ boot screens

static void drawSplash(const String& line1, const String& line2 = "", uint16_t color2 = UI_DIM) {
    tft.fillScreen(UI_BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(UI_TEXT);
    tft.drawString("UniFi Monitor", UI_W / 2, UI_H / 2 - 30, UI_FONT_BIG);
    tft.setTextColor(UI_DIM);
    tft.drawString(line1, UI_W / 2, UI_H / 2 + 6, UI_FONT_BODY);
    if (line2.length()) {
        tft.setTextColor(color2);
        tft.drawString(line2, UI_W / 2, UI_H / 2 + 28, UI_FONT_BODY);
    }
    tft.setTextColor(UI_MUTED);
    tft.drawString("v" FIRMWARE_VERSION, UI_W / 2, UI_H - 12, UI_FONT_SMALL);
    tft.setTextDatum(TL_DATUM);
}

// Give the user a moment to hold the screen to re-open setup
static bool holdForSetup() {
    drawSplash("Starting...", "Hold the screen to open setup");
    unsigned long start = millis();
    unsigned long heldSince = 0;
    while (millis() - start < 2500) {
        if (touchHeld()) {
            if (!heldSince) heldSince = millis();
            if (millis() - heldSince > 1000) return true;
        } else {
            heldSince = 0;
        }
        delay(20);
    }
    return false;
}

static bool connectWiFi() {
    drawSplash("Connecting to " + settings.wifiSsid + "...");
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPass.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
        delay(250);
    }
    return WiFi.status() == WL_CONNECTED;
}

// ====================================================================== setup

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\nUniFi Monitor " FIRMWARE_VERSION " - " BOARD_NAME);

    bool haveSettings = settingsLoad(settings);
#ifdef HAVE_BUILTIN_CREDENTIALS
    if (!haveSettings) {
        // Developer shortcut: seed the saved settings from include/credentials.h once
        settings.wifiSsid = WIFI_SSID;
        settings.wifiPass = WIFI_PASSWORD;
        settings.unifiHost = UNIFI_HOST;
        settings.unifiUser = UNIFI_USERNAME;
        settings.unifiPass = UNIFI_PASSWORD;
        settingsSave(settings);
        haveSettings = true;
    }
#endif
    rotation = settings.rotate180 ? (UI_ROTATION + 2) % 4 : UI_ROTATION;

    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
    tft.init();
    tft.setRotation(rotation);
    if (settings.invertColors) tft.invertDisplay(true);
    tft.fillScreen(UI_BG);

    touchBegin();
    pagesInit(&unifiAPI);

    if (!settings.isComplete()) {
        runSetupPortal(settings, "", 0);   // first run: wait for the user
    }
    if (holdForSetup()) {
        runSetupPortal(settings, "", SETUP_PORTAL_TIMEOUT_MS);
    }
    if (!connectWiFi()) {
        runSetupPortal(settings, "Can't join WiFi \"" + settings.wifiSsid + "\"", SETUP_PORTAL_TIMEOUT_MS);
    }
    Serial.print("WiFi connected, IP ");
    Serial.println(WiFi.localIP());

    configTzTime(settings.timezone.c_str(), NTP_SERVER);

    drawSplash("Signing in to UniFi...");
    unifiAPI.configure(settings.unifiHost, settings.unifiUser, settings.unifiPass);
    lastLoginAttempt = millis();
    if (!unifiAPI.authenticate()) {
        drawSplash("UniFi sign-in failed", unifiAPI.lastError(), UI_WARN);
        delay(4000);
    } else {
        unifiAPI.fetchExperience(currentStats.experience);
        lastExperienceUpdate = millis();
    }

    tft.fillScreen(UI_BG);
    drawHeader();
    drawTabBar();
    tabNeedsRedraw = true;
}

// ======================================================================= loop

void loop() {
    unsigned long now = millis();
    handleTouch();

    if (currentTab != prevTab || tabNeedsRedraw) {
        prevTab = currentTab;
        tabNeedsRedraw = false;
        tft.fillRect(0, CONTENT_START_Y, SCREEN_WIDTH, SCREEN_HEIGHT - CONTENT_START_Y, UI_BG);
        // Free RAM held by whichever screen we left (each HTTPS fetch needs ~45 KB)
        if (currentTab == TAB_OVERVIEW) {
            pagesReleaseAll(0);
            drawStaticElements();
        } else {
            releaseGraphSprite();
            pagesShow(currentTab);
        }
    }

    // Header clock / status dot, once a minute (or right after a login change)
    static int lastHeaderMinute = -1;
    static unsigned long lastHeaderCheck = 0;
    if (now - lastHeaderCheck >= 1000) {
        lastHeaderCheck = now;
        int minute = (time(nullptr) / 60) % 60;
        if (minute != lastHeaderMinute) {
            lastHeaderMinute = minute;
            drawHeader();
        }
    }

    // Serial debug: '1'-'5' tab, 'o' open first row, 'n' next page, 'b' back, 's' screenshot
    while (Serial.available()) {
        char c = Serial.read();
        if (c >= '1' && c < '1' + TAB_COUNT) {
            currentTab = c - '0';
            tabNeedsRedraw = true;
            drawTabBar();
        } else if (currentTab != TAB_OVERVIEW && c == 'o') {
            pagesTouch(currentTab, 100, 70);
        } else if (currentTab != TAB_OVERVIEW && c == 'n') {
            pagesTouch(currentTab, UI_W - 110, UI_H - 15);
        } else if (currentTab != TAB_OVERVIEW && c == 'b') {
            pagesTouch(currentTab, 20, UI_H - 15);
        } else if (c == 's') {
            // Screenshot: "SCRN" + UI_W x UI_H RGB565 rows read back from the panel
            static uint16_t row[UI_W];
            Serial.flush();
            Serial.write((const uint8_t*)"SCRN", 4);
            for (int y = 0; y < UI_H; y++) {
                tft.readRect(0, y, UI_W, 1, row);
                Serial.write((const uint8_t*)row, sizeof(row));
            }
            Serial.flush();
        }
    }
    if (tabNeedsRedraw) return;

    // Retry the UniFi login after failures (wrong password, console rebooting...)
    if (!unifiAPI.isConnected() && WiFi.status() == WL_CONNECTED && now - lastLoginAttempt > UNIFI_RETRY_MS) {
        lastLoginAttempt = now;
        unifiAPI.authenticate();
        drawHeader();
    }

    if (currentTab != TAB_OVERVIEW) {
        pagesLoop(currentTab);
        delay(1);
        return;
    }

    // ---- Overview ----
    if (now - lastApiUpdate >= OVERVIEW_POLL_MS && unifiAPI.isConnected()) {
        lastApiUpdate = now;
        if (unifiAPI.fetchOverview(currentStats)) {
            targetDownload = currentStats.download_mbps;
            targetUpload = currentStats.upload_mbps;
        }
        if (now - lastExperienceUpdate >= EXPERIENCE_POLL_MS) {
            lastExperienceUpdate = now;
            unifiAPI.fetchExperience(currentStats.experience);
        }
    }
    if (now - lastDisplayUpdate >= DISPLAY_UPDATE_INTERVAL) {
        lastDisplayUpdate = now;
        smoothDownload += (targetDownload - smoothDownload) * 0.35f;
        smoothUpload += (targetUpload - smoothUpload) * 0.35f;
        updateSpeedDisplay();
    }
    if (now - lastGraphScroll >= GRAPH_SCROLL_INTERVAL) {
        lastGraphScroll = now;
        scrollGraphData();
        updateGraphDisplay();
    }
    if (now - lastStatsRedraw >= STATS_REDRAW_INTERVAL) {
        lastStatsRedraw = now;
        updateStatsBar();
    }
    delay(1);
}

void handleTouch() {
    int x = 0, y = 0;
    unsigned long now = millis();
    bool pressed = touchRead(x, y);

    if (!pressed && now - lastTouchTime > 300) touchPressed = false;
    if (!pressed || touchPressed || now - lastTouchTime <= TOUCH_DEBOUNCE_MS) return;

    touchPressed = true;
    lastTouchTime = now;
    if (y < CONTENT_START_Y + 2) {
        int newTab = x / (SCREEN_WIDTH / TAB_COUNT) + 1;
        if (newTab >= 1 && newTab <= TAB_COUNT && newTab != currentTab) {
            currentTab = newTab;
            tabNeedsRedraw = true;
            drawTabBar();
        }
    } else if (currentTab != TAB_OVERVIEW) {
        pagesTouch(currentTab, x, y);
    }
}

// =================================================================== drawing

// Draw text centered in [left, left+width) using the biggest font that fits
static void drawFitted(const String& text, int left, int width, int y, uint8_t font, uint16_t color) {
    uint8_t f = font;
    if (f == UI_FONT_BIG && tft.textWidth(text, f) > width - 4) f = UI_FONT_BODY;
    int yOff = (f != font) ? 5 : 0;
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(color);
    tft.drawString(text, left + width / 2, y + yOff, f);
    tft.setTextDatum(TL_DATUM);
}

static void drawLabel(const char* text, int cx, int y, uint16_t color = UI_DIM) {
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(color);
    tft.drawString(text, cx, y, UI_FONT_SMALL);
    tft.setTextDatum(TL_DATUM);
}

static void drawCard(int x, int y, int w, int h) {
    tft.fillRoundRect(x, y, w, h, 6, UI_CARD);
    tft.drawRoundRect(x, y, w, h, 6, UI_CARD_EDGE);
}

// 1 decimal below 100, none above, so values fit narrow cards
static String fitNumber(float v) {
    return (v >= 99.95f) ? String((long)(v + 0.5f)) : String(v, 1);
}

static uint16_t levelColor(float pct) {
    if (pct >= 90) return UI_GOOD;
    if (pct >= 70) return UI_WARN;
    return UI_BAD;
}

void drawHeader() {
    tft.fillRect(0, 0, SCREEN_WIDTH, UI_HEADER_H, UI_BG);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(UI_TEXT);
    tft.drawString("UniFi Monitor", 6, 1, UI_FONT_BODY);

    String clock = "--:--";
    time_t t = time(nullptr);
    if (t > 1700000000) {
        struct tm tmv;
        localtime_r(&t, &tmv);
        char buf[12];
        if (settings.clock24h) {
            snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        } else {
            int h12 = tmv.tm_hour % 12;
            snprintf(buf, sizeof(buf), "%d:%02d %s", h12 ? h12 : 12, tmv.tm_min, tmv.tm_hour < 12 ? "AM" : "PM");
        }
        clock = buf;
    }
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(UI_DIM);
    tft.drawString(clock, SCREEN_WIDTH - 6, 1, UI_FONT_BODY);
    int clockW = tft.textWidth(clock, UI_FONT_BODY);
    bool online = WiFi.status() == WL_CONNECTED && unifiAPI.isConnected();
    tft.fillCircle(SCREEN_WIDTH - 6 - clockW - 9, 9, 4, online ? UI_GOOD : UI_BAD);
    tft.setTextDatum(TL_DATUM);
}

void drawTabBar() {
    int tabWidth = SCREEN_WIDTH / TAB_COUNT;
    tft.fillRect(0, UI_TAB_Y, SCREEN_WIDTH, UI_TAB_H, UI_BG);
    tft.setTextDatum(MC_DATUM);
    for (int i = 1; i <= TAB_COUNT; i++) {
        int x = (i - 1) * tabWidth;
        bool sel = (i == currentTab);
        tft.setTextColor(sel ? UI_TEXT : UI_MUTED);
        tft.drawString(TAB_NAMES[i - 1], x + tabWidth / 2, UI_TAB_Y + 10, UI_FONT_BODY);
        if (sel) tft.fillRect(x + 8, UI_TAB_Y + UI_TAB_H - 3, tabWidth - 16, 2, UI_ACCENT);
    }
    tft.setTextDatum(TL_DATUM);
    tft.drawFastHLine(0, UI_CONTENT_Y - 1, SCREEN_WIDTH, UI_CARD_EDGE);
}

// Vertical positions inside the speed card: two equal halves, each holding
// label (8px) + value (26px) + unit (8px), centred
#define SPEED_HALF        (SPEED_BOX_HEIGHT / 2)
#define SPEED_TOP(half)   (SPEED_BOX_Y + (half) * SPEED_HALF + (SPEED_HALF - 47) / 2)
#define STAT_PAD          ((STATS_HEIGHT - 67) / 2)

void drawStaticElements() {
    drawCard(GRAPH_X, GRAPH_Y, GRAPH_WIDTH, GRAPH_HEIGHT);
    tft.setTextColor(UI_TEXT);
    tft.drawString("Throughput", GRAPH_X + 8, GRAPH_Y + 3, UI_FONT_BODY);
    int lx = GRAPH_X + GRAPH_WIDTH - 76;
    tft.fillRect(lx, GRAPH_Y + 10, 8, 2, UI_DOWN);
    tft.setTextColor(UI_DIM);
    tft.drawString("Down", lx + 11, GRAPH_Y + 7, UI_FONT_SMALL);
    tft.fillRect(lx + 42, GRAPH_Y + 10, 8, 2, UI_UP);
    tft.drawString("Up", lx + 53, GRAPH_Y + 7, UI_FONT_SMALL);

    drawCard(SPEED_BOX_X, SPEED_BOX_Y, SPEED_BOX_WIDTH, SPEED_BOX_HEIGHT);
    int cx = SPEED_BOX_X + SPEED_BOX_WIDTH / 2;
    drawLabel("DOWNLOAD", cx, SPEED_TOP(0));
    drawLabel("Mbps", cx, SPEED_TOP(0) + 39);
    tft.drawFastHLine(SPEED_BOX_X + 10, SPEED_BOX_Y + SPEED_HALF, SPEED_BOX_WIDTH - 20, UI_CARD_EDGE);
    drawLabel("UPLOAD", cx, SPEED_TOP(1));
    drawLabel("Mbps", cx, SPEED_TOP(1) + 39);

    const char* labels[4] = {"CLIENTS", "PING", "EXPERIENCE", "WAN UPTIME"};
    for (int i = 0; i < 4; i++) {
        drawCard(STAT_X(i), STATS_Y, STAT_BOX_WIDTH, STATS_HEIGHT);
        drawLabel(labels[i], STAT_X(i) + STAT_BOX_WIDTH / 2, STATS_Y + STAT_PAD + 8);
    }

    prevDrawnDownload = -1;
    prevDrawnUpload = -1;
    lastStatsRedraw = 0;
    updateGraphDisplay();
}

void updateSpeedDisplay() {
    bool downloadChanged = fabsf(smoothDownload - prevDrawnDownload) >= 0.05f;
    bool uploadChanged = fabsf(smoothUpload - prevDrawnUpload) >= 0.05f;
    if (!downloadChanged && !uploadChanged) return;

    int innerX = SPEED_BOX_X + 3;
    int innerW = SPEED_BOX_WIDTH - 6;
    if (downloadChanged) {
        prevDrawnDownload = smoothDownload;
        tft.fillRect(innerX, SPEED_TOP(0) + 10, innerW, 27, UI_CARD);
        drawFitted(fitNumber(smoothDownload), innerX, innerW, SPEED_TOP(0) + 11, UI_FONT_BIG, UI_DOWN);
    }
    if (uploadChanged) {
        prevDrawnUpload = smoothUpload;
        tft.fillRect(innerX, SPEED_TOP(1) + 10, innerW, 27, UI_CARD);
        drawFitted(fitNumber(smoothUpload), innerX, innerW, SPEED_TOP(1) + 11, UI_FONT_BIG, UI_UP);
    }
}

void updateGraphDisplay() {
    const int px = GRAPH_X + 6, py = GRAPH_Y + 22;
    const int pw = GRAPH_WIDTH - 12, ph = GRAPH_HEIGHT - 28;

    // Off-screen sprite avoids flicker; only if it leaves enough RAM for HTTPS
    if (!graphSpriteOk && ESP.getMaxAllocHeap() > (uint32_t)(pw * ph * 2) + 60000) {
        graphSprite.setColorDepth(16);
        graphSpriteOk = graphSprite.createSprite(pw, ph) != nullptr;
    }
    TFT_eSPI* g = graphSpriteOk ? (TFT_eSPI*)&graphSprite : &tft;
    int ox = graphSpriteOk ? 0 : px, oy = graphSpriteOk ? 0 : py;
    g->fillRect(ox, oy, pw, ph, UI_CARD);

    float maxVal = 1.0;
    for (int i = 0; i < GRAPH_POINTS; i++) maxVal = max(maxVal, max(downloadHistory[i], uploadHistory[i]));
    const float steps[] = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000};
    float scale = steps[sizeof(steps) / sizeof(steps[0]) - 1];
    for (float s : steps) { if (maxVal * 1.1f <= s) { scale = s; break; } }

    for (int i = 0; i <= 3; i++) {
        int y = oy + (ph - 1) * i / 3;
        for (int x = ox; x < ox + pw; x += 4) g->drawPixel(x, y, UI_CARD_EDGE);
    }
    g->setTextColor(UI_MUTED);
    g->setTextDatum(TR_DATUM);
    g->drawString(String((long)scale), ox + pw - 1, oy + 2, UI_FONT_SMALL);

    auto yOf = [&](float v) { return oy + ph - 1 - (int)(min(v, scale) * (ph - 2) / scale); };
    for (int i = 1; i < GRAPH_POINTS; i++) {
        int x1 = ox + (i - 1) * (pw - 1) / (GRAPH_POINTS - 1);
        int x2 = ox + i * (pw - 1) / (GRAPH_POINTS - 1);
        g->drawLine(x1, yOf(uploadHistory[i - 1]), x2, yOf(uploadHistory[i]), UI_UP);
        g->drawLine(x1, yOf(downloadHistory[i - 1]), x2, yOf(downloadHistory[i]), UI_DOWN);
    }

    // Explain an empty graph instead of showing a flat line
    if (!unifiAPI.isConnected() && unifiAPI.lastError().length()) {
        g->setTextDatum(MC_DATUM);
        g->setTextColor(UI_WARN);
        g->drawString(unifiAPI.lastError(), ox + pw / 2, oy + ph / 2, UI_FONT_SMALL);
    }
    g->setTextDatum(TL_DATUM);
    if (graphSpriteOk) graphSprite.pushSprite(px, py);
}

void scrollGraphData() {
    for (int i = 0; i < GRAPH_POINTS - 1; i++) {
        downloadHistory[i] = downloadHistory[i + 1];
        uploadHistory[i] = uploadHistory[i + 1];
    }
    downloadHistory[GRAPH_POINTS - 1] = smoothDownload;
    uploadHistory[GRAPH_POINTS - 1] = smoothUpload;
}

void updateStatsBar() {
    const NetworkStats& s = currentStats;
    for (int i = 0; i < 4; i++) {
        String value = "--", unit;
        uint16_t color = UI_DIM;
        switch (i) {
            case 0:
                if (s.clients >= 0) { value = String(s.clients); color = UI_TEXT; }
                unit = "connected";
                break;
            case 1:
                if (s.ping_ms >= 0) {
                    value = fitNumber(s.ping_ms);
                    color = s.ping_ms < 30 ? UI_GOOD : s.ping_ms < 80 ? UI_WARN : UI_BAD;
                }
                unit = "ms";
                break;
            case 2:
                if (s.experience >= 0) { value = String((long)(s.experience + 0.5f)); color = levelColor(s.experience); }
                unit = "%";
                break;
            case 3:
                if (s.uptime_percent >= 0) { value = fitNumber(s.uptime_percent); color = levelColor(s.uptime_percent); }
                unit = "%";
                break;
        }
        int x = STAT_X(i);
        int top = STATS_Y + STAT_PAD;
        tft.fillRect(x + 3, top + 19, STAT_BOX_WIDTH - 6, 40, UI_CARD);
        drawFitted(value, x + 3, STAT_BOX_WIDTH - 6, top + 20, UI_FONT_BIG, color);
        drawLabel(unit.c_str(), x + STAT_BOX_WIDTH / 2, top + 50);
    }
}
