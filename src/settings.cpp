// Saved settings + the first-run setup hotspot.
//
// The setup page is a tiny captive portal: join the "UniFi-Monitor-XXXX" WiFi
// network from a phone and the page pops up (or browse to http://192.168.4.1).
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <TFT_eSPI.h>
#include "settings.h"
#include "config.h"
#include "theme.h"

extern TFT_eSPI tft;

static const char* NS = "unifimon";

bool settingsLoad(Settings& s) {
    Preferences p;
    if (!p.begin(NS, true)) return false;
    bool has = p.isKey("ssid");
    if (has) {
        s.wifiSsid = p.getString("ssid", "");
        s.wifiPass = p.getString("wpass", "");
        s.unifiHost = p.getString("host", "");
        s.unifiUser = p.getString("user", "");
        s.unifiPass = p.getString("upass", "");
        s.timezone = p.getString("tz", s.timezone);
        s.clock24h = p.getBool("clk24", false);
        s.rotate180 = p.getBool("rot180", false);
        s.invertColors = p.getBool("invert", false);
    }
    p.end();
    return has;
}

void settingsSave(const Settings& s) {
    Preferences p;
    p.begin(NS, false);
    p.putString("ssid", s.wifiSsid);
    p.putString("wpass", s.wifiPass);
    p.putString("host", s.unifiHost);
    p.putString("user", s.unifiUser);
    p.putString("upass", s.unifiPass);
    p.putString("tz", s.timezone);
    p.putBool("clk24", s.clock24h);
    p.putBool("rot180", s.rotate180);
    p.putBool("invert", s.invertColors);
    p.end();
}

bool settingsLoadTouchCal(uint16_t cal[5]) {
    Preferences p;
    if (!p.begin(NS, true)) return false;
    bool ok = p.getBytesLength("tcal") == sizeof(uint16_t) * 5 && p.getBytes("tcal", cal, sizeof(uint16_t) * 5) > 0;
    p.end();
    return ok;
}

void settingsSaveTouchCal(const uint16_t cal[5]) {
    Preferences p;
    p.begin(NS, false);
    p.putBytes("tcal", cal, sizeof(uint16_t) * 5);
    p.end();
}

void settingsClearTouchCalibration() {
    Preferences p;
    p.begin(NS, false);
    p.remove("tcal");
    p.end();
}

// ------------------------------------------------------------------ portal

struct TzOption { const char* label; const char* tz; };
static const TzOption TIMEZONES[] = {
    {"US Eastern", "EST5EDT,M3.2.0,M11.1.0"},
    {"US Central", "CST6CDT,M3.2.0,M11.1.0"},
    {"US Mountain", "MST7MDT,M3.2.0,M11.1.0"},
    {"US Arizona", "MST7"},
    {"US Pacific", "PST8PDT,M3.2.0,M11.1.0"},
    {"US Alaska", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"US Hawaii", "HST10"},
    {"Atlantic (Canada)", "AST4ADT,M3.2.0,M11.1.0"},
    {"UK / Ireland", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Eastern Europe", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"India", "IST-5:30"},
    {"China / Singapore", "CST-8"},
    {"Japan / Korea", "JST-9"},
    {"Australia East", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia West", "AWST-8"},
    {"New Zealand", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"UTC", "UTC0"},
};

static String htmlEscape(const String& in) {
    String out;
    out.reserve(in.length() + 8);
    for (char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

static void drawPortalScreen(const String& apName, const String& reason) {
    tft.fillScreen(UI_BG);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(UI_ACCENT);
    tft.drawString("Setup", 10, 8, UI_FONT_BIG);
    tft.setTextColor(UI_DIM);
    tft.drawString("UniFi Monitor " FIRMWARE_VERSION, 10, 38, UI_FONT_SMALL);

    int y = 58;
    if (reason.length()) {
        tft.setTextColor(UI_WARN);
        tft.drawString(reason, 10, y, UI_FONT_BODY);
        y += 24;
    }
    tft.setTextColor(UI_TEXT);
    tft.drawString("1. On your phone, join WiFi:", 10, y, UI_FONT_BODY);
    tft.setTextColor(UI_ACCENT);
    tft.drawString(apName, 28, y + 20, UI_FONT_BIG);
    tft.setTextColor(UI_TEXT);
    tft.drawString("2. The setup page opens by itself,", 10, y + 54, UI_FONT_BODY);
    tft.drawString("   or browse to  http://192.168.4.1", 10, y + 72, UI_FONT_BODY);
    tft.setTextColor(UI_DIM);
    tft.drawString("Enter your WiFi + UniFi login, tap Save.", 10, y + 98, UI_FONT_BODY);
}

static String buildPage(const Settings& s, const String& networksHtml) {
    String tzOptions;
    for (const auto& t : TIMEZONES) {
        tzOptions += "<option value=\"" + String(t.tz) + "\"";
        if (s.timezone == t.tz) tzOptions += " selected";
        tzOptions += ">" + String(t.label) + "</option>";
    }
    bool knownTz = false;
    for (const auto& t : TIMEZONES) if (s.timezone == t.tz) knownTz = true;
    if (!knownTz && s.timezone.length())
        tzOptions += "<option value=\"" + htmlEscape(s.timezone) + "\" selected>" + htmlEscape(s.timezone) + "</option>";

    String page = F("<!doctype html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>UniFi Monitor setup</title><style>"
        "body{font-family:system-ui,sans-serif;background:#0d1117;color:#e6edf3;margin:0;padding:16px}"
        "main{max-width:420px;margin:auto}h1{font-size:22px;margin:4px 0 2px}"
        ".sub{color:#8b949e;font-size:13px;margin-bottom:12px}"
        "fieldset{border:1px solid #30363d;border-radius:10px;margin:14px 0;padding:10px 14px 14px}"
        "legend{color:#3fb9f8;padding:0 6px}label{display:block;font-size:13px;color:#8b949e;margin-top:10px}"
        "input,select{width:100%;box-sizing:border-box;padding:10px;margin-top:4px;border-radius:8px;"
        "border:1px solid #30363d;background:#161b22;color:#e6edf3;font-size:16px}"
        ".chk{display:flex;align-items:center;gap:8px;color:#e6edf3;font-size:15px}"
        ".chk input{width:auto;margin:0}"
        "button{width:100%;padding:14px;margin-top:16px;border:0;border-radius:10px;background:#1f6feb;"
        "color:#fff;font-size:17px;font-weight:600}.hint{font-size:12px;color:#8b949e;margin-top:4px}"
        "</style></head><body><main><h1>UniFi Monitor</h1><div class=sub>");
    page += F("Firmware " FIRMWARE_VERSION " &middot; " BOARD_NAME "</div><form method=post action=/save>");

    page += F("<fieldset><legend>WiFi (2.4 GHz)</legend><label>Network</label>"
              "<input name=ssid list=nets required value=\"");
    page += htmlEscape(s.wifiSsid);
    page += F("\"><datalist id=nets>");
    page += networksHtml;
    page += F("</datalist><label>Password</label><input name=wpass type=password placeholder=\"");
    page += s.wifiPass.length() ? F("(unchanged)") : F("");
    page += F("\"></fieldset>");

    page += F("<fieldset><legend>UniFi console</legend><label>Console IP or hostname</label>"
              "<input name=host required placeholder=192.168.1.1 value=\"");
    page += htmlEscape(s.unifiHost);
    page += F("\"><div class=hint>Your UDM / Cloud Key / gateway running UniFi OS.</div>"
              "<label>Local admin username</label><input name=user required autocapitalize=off value=\"");
    page += htmlEscape(s.unifiUser);
    page += F("\"><div class=hint>Use a <b>local-only</b> admin (Settings &rarr; Admins &amp; Users), "
              "not your ui.com account.</div><label>Password</label><input name=upass type=password placeholder=\"");
    page += s.unifiPass.length() ? F("(unchanged)") : F("");
    page += F("\"></fieldset>");

    page += F("<fieldset><legend>Display</legend><label>Time zone</label><select name=tz>");
    page += tzOptions;
    page += F("</select><label class=chk><input type=checkbox name=clk24");
    if (s.clock24h) page += F(" checked");
    page += F("> 24-hour clock</label><label class=chk><input type=checkbox name=rot180");
    if (s.rotate180) page += F(" checked");
    page += F("> Rotate screen 180&deg;</label><label class=chk><input type=checkbox name=invert");
    if (s.invertColors) page += F(" checked");
    page += F("> Invert colors (if black looks white)</label>");
#ifdef TOUCH_CS
    page += F("<label class=chk><input type=checkbox name=recal> Recalibrate touch on next start</label>");
#endif
    page += F("</fieldset><button type=submit>Save &amp; restart</button></form></main></body></html>");
    return page;
}

void runSetupPortal(const Settings& current, const String& reason, unsigned long timeoutMs) {
    // Scan first (needs STA mode), then start the hotspot
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks();
    String networks;
    for (int i = 0; i < n && i < 25; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() && networks.indexOf("\"" + htmlEscape(ssid) + "\"") < 0)
            networks += "<option value=\"" + htmlEscape(ssid) + "\">";
    }
    WiFi.scanDelete();

    uint8_t mac[6];
    WiFi.macAddress(mac);
    char apName[32];
    snprintf(apName, sizeof(apName), "UniFi-Monitor-%02X%02X", mac[4], mac[5]);

    WiFi.mode(WIFI_AP);
    WiFi.softAP(apName);
    delay(200);
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("Setup portal: join '%s', open http://%s\n", apName, ip.toString().c_str());
    drawPortalScreen(apName, reason);

    DNSServer dns;
    dns.start(53, "*", ip);
    WebServer server(80);
    Settings working = current;
    bool saved = false;

    server.on("/", HTTP_GET, [&]() { server.send(200, "text/html", buildPage(working, networks)); });
    server.on("/save", HTTP_POST, [&]() {
        working.wifiSsid = server.arg("ssid");
        working.wifiSsid.trim();
        if (server.arg("wpass").length()) working.wifiPass = server.arg("wpass");
        working.unifiHost = server.arg("host");
        working.unifiHost.trim();
        working.unifiUser = server.arg("user");
        working.unifiUser.trim();
        if (server.arg("upass").length()) working.unifiPass = server.arg("upass");
        if (server.arg("tz").length()) working.timezone = server.arg("tz");
        working.clock24h = server.hasArg("clk24");
        working.rotate180 = server.hasArg("rot180");
        working.invertColors = server.hasArg("invert");
        // Touch calibration is tied to the screen orientation
        if (server.hasArg("recal") || working.rotate180 != current.rotate180) settingsClearTouchCalibration();
        settingsSave(working);
        server.send(200, "text/html",
            F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
              "<body style='font-family:system-ui;background:#0d1117;color:#e6edf3;padding:24px'>"
              "<h2>Saved</h2><p>The monitor is restarting and will connect to your network.</p></body>"));
        saved = true;
    });
    // Captive-portal: send every other URL (phone connectivity checks) to the form
    server.onNotFound([&]() {
        server.sendHeader("Location", String("http://") + ip.toString() + "/", true);
        server.send(302, "text/plain", "");
    });
    server.begin();

    unsigned long start = millis();
    while (true) {
        dns.processNextRequest();
        server.handleClient();
        if (saved) {
            delay(1500);
            tft.fillScreen(UI_BG);
            tft.setTextDatum(MC_DATUM);
            tft.setTextColor(UI_GOOD);
            tft.drawString("Saved - restarting", UI_W / 2, UI_H / 2, UI_FONT_BODY);
            delay(800);
            ESP.restart();
        }
        if (timeoutMs && millis() - start > timeoutMs) {
            Serial.println("Setup portal timed out, restarting to retry");
            ESP.restart();
        }
        delay(2);
    }
}
