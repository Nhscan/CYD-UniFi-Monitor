// List + detail pages for the Devices, Clients, Firewall and Alerts tabs.
//
// Screen layout below the tab bar (320x240, landscape):
//   y  48..68   title bar   (page name + summary)
//   y  70..210  7 list rows of 20 px  /  detail lines
//   y 212..238  nav buttons (Prev / page / Next / Refresh, or Back in detail view)
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <time.h>
#include "ui_pages.h"
#include "theme.h"

extern TFT_eSPI tft;

const char* const TAB_NAMES[TAB_COUNT] = {"Overview", "Devices", "Clients", "Firewall", "Alerts"};

// ---- Layout ----
static const int SCR_W = UI_W;
static const int CONTENT_Y = UI_CONTENT_Y;   // 41
static const int TITLE_Y = 42, TITLE_H = 18;
static const int NAV_H = 26;
static const int NAV_Y = UI_H - NAV_H - 6;                  // 208 on a 240-high screen
static const int ROW_Y = 62, ROW_H = 20;
static const int ROWS = (NAV_Y - 6 - ROW_Y) / ROW_H;         // 7 on 240, 11 on 320
static const int DETAIL_LINE_H = 17;
static const int DETAIL_LINES = (NAV_Y - 8 - ROW_Y) / DETAIL_LINE_H;

// Nav buttons: [< Prev] page [Next >] ........ [Refresh]   (right-aligned on wide screens)
static const int BTN_PREV_X = 4, BTN_PREV_W = 64;
static const int BTN_REFRESH_W = 66, BTN_REFRESH_X = SCR_W - 4 - BTN_REFRESH_W;
static const int BTN_NEXT_W = 64, BTN_NEXT_X = BTN_REFRESH_X - 20 - BTN_NEXT_W;
static const int PAGE_LABEL_X = (BTN_PREV_X + BTN_PREV_W + BTN_NEXT_X) / 2;
static const int BTN_BACK_W = 70;
static const int BTN_DPREV_W = 56, BTN_DPREV_X = BTN_NEXT_X - 58;
static const int FONT = UI_FONT_BODY;        // 16 px proportional
static const int LABEL_W = 92;               // label column in detail view

// ---- Colors (shared theme) ----
static const uint16_t C_BG = UI_BG;
static const uint16_t C_PANEL = UI_CARD;
static const uint16_t C_ROW_ALT = UI_ROW_ALT;
static const uint16_t C_BORDER = UI_CARD_EDGE;
static const uint16_t C_TEXT = UI_TEXT;
static const uint16_t C_DIM = UI_DIM;
static const uint16_t C_MUTED = UI_MUTED;
static const uint16_t C_ACCENT = UI_ACCENT;
static const uint16_t C_GOOD = UI_GOOD;
static const uint16_t C_WARN = UI_WARN;
static const uint16_t C_BAD = UI_BAD;
static const uint16_t C_ORANGE = UI_ORANGE;

// ---- Refresh intervals ----
static const unsigned long REFRESH_MS[TAB_COUNT + 1] = {0, 0, 30000, 30000, 60000, 60000};

// ---- Data ----
static UniFiAPI* api = nullptr;
static std::vector<UDevice> devices;
static std::vector<UClient> clients;
static UFirewallInfo firewall;
static std::vector<UAlert> alerts;
static int alertsTotal = 0;

struct PageState {
    int page = 0;              // list page
    int selected = -1;         // >= 0 -> detail view of this item
    int detailPage = 0;
    int detailPages = 1;
    unsigned long lastFetch = 0;
    bool loaded = false;
    bool lastOk = true;
};
static PageState ps[TAB_COUNT + 1];

// =============================================================== formatting

static String fmtDuration(uint32_t s) {
    uint32_t d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
    if (d > 0) return String(d) + "d " + String(h) + "h";
    if (h > 0) return String(h) + "h " + String(m) + "m";
    return String(m) + "m";
}

static String fmtBytes(double b) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int u = 0;
    while (b >= 1024.0 && u < 4) { b /= 1024.0; u++; }
    return String(b, u == 0 ? 0 : 1) + " " + units[u];
}

static String fmtCount(double n) {
    if (n >= 1e9) return String(n / 1e9, 1) + "B";
    if (n >= 1e6) return String(n / 1e6, 1) + "M";
    if (n >= 1e4) return String(n / 1e3, 0) + "K";
    if (n >= 1e3) return String(n / 1e3, 1) + "K";
    return String((long)n);
}

static String fmtSpeed(int mbps) {
    if (mbps <= 0) return "";
    if (mbps >= 1000) return String(mbps / 1000.0, mbps % 1000 ? 1 : 0) + "G";
    return String(mbps) + "M";
}

static bool clockValid() { return time(nullptr) > 1700000000; }

static String fmtAgo(double epochMs) {
    if (epochMs <= 0 || !clockValid()) return "";
    long diff = (long)(time(nullptr) - (time_t)(epochMs / 1000.0));
    if (diff < 0) diff = 0;
    if (diff < 60) return "now";
    if (diff < 3600) return String(diff / 60) + "m";
    if (diff < 86400) return String(diff / 3600) + "h";
    return String(diff / 86400) + "d";
}

static String fmtDateTime(double epochMs) {
    if (epochMs <= 0) return "-";
    time_t t = (time_t)(epochMs / 1000.0);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char date[12], buf[32];
    strftime(date, sizeof(date), "%b %d", &tmv);
    int h12 = tmv.tm_hour % 12;
    snprintf(buf, sizeof(buf), "%s %d:%02d %s", date, h12 ? h12 : 12, tmv.tm_min, tmv.tm_hour < 12 ? "AM" : "PM");
    String s(buf);
    String ago = fmtAgo(epochMs);
    if (ago.length()) s += " (" + ago + (ago == "now" ? ")" : " ago)");
    return s;
}

static String deviceTypeName(const String& t) {
    if (t == "uxg" || t == "ugw" || t == "usg") return "Gateway";
    if (t == "udm") return "Console / Gateway";
    if (t == "uap") return "Access Point";
    if (t == "usw") return "Switch";
    return t;
}

static String deviceStateName(int s) {
    switch (s) {
        case 0: return "Offline";
        case 1: return "Online";
        case 2: return "Pending adoption";
        case 4: return "Upgrading";
        case 5: return "Provisioning";
        case 6: return "Heartbeat missed";
        case 11: return "Isolated";
        default: return "State " + String(s);
    }
}

static uint16_t signalColor(int dbm) {
    if (dbm == 0) return C_DIM;
    if (dbm >= -60) return C_GOOD;
    if (dbm >= -70) return C_WARN;
    return C_BAD;
}

static String signalWord(int dbm) {
    if (dbm >= -50) return "Excellent";
    if (dbm >= -60) return "Good";
    if (dbm >= -70) return "Fair";
    return "Poor";
}

static uint16_t severityColor(const String& s) {
    if (s == "VERY_HIGH" || s == "CRITICAL") return C_BAD;
    if (s == "HIGH") return C_ORANGE;
    if (s == "MEDIUM") return C_WARN;
    return C_DIM;
}

static String severityName(const String& s) {
    if (s == "VERY_HIGH") return "Critical";
    if (s == "HIGH") return "High";
    if (s == "MEDIUM") return "Medium";
    if (s == "LOW") return "Low";
    return s;
}

static String prettyCategory(String c) {
    c.replace("_", " ");
    c.toLowerCase();
    if (c.length()) c.setCharAt(0, toupper(c[0]));
    return c;
}

// Truncate text with ".." so it fits maxW pixels in FONT
static String fitText(const String& s, int maxW) {
    if (tft.textWidth(s, FONT) <= maxW) return s;
    String t = s;
    while (t.length() > 1 && tft.textWidth(t + "..", FONT) > maxW) t.remove(t.length() - 1);
    return t + "..";
}

// =============================================================== drawing helpers

static void drawText(const String& s, int x, int y, uint16_t color, uint8_t datum = TL_DATUM) {
    tft.setTextDatum(datum);
    tft.setTextColor(color);
    tft.drawString(s, x, y, FONT);
    tft.setTextDatum(TL_DATUM);
}

static void drawButton(int x, int w, const String& label, bool enabled) {
    tft.fillRoundRect(x, NAV_Y, w, NAV_H, 6, C_PANEL);
    tft.drawRoundRect(x, NAV_Y, w, NAV_H, 6, C_BORDER);
    drawText(label, x + w / 2, NAV_Y + NAV_H / 2, enabled ? C_TEXT : C_MUTED, MC_DATUM);
}

static void clearContent() {
    tft.fillRect(0, CONTENT_Y, SCR_W, UI_H - CONTENT_Y, C_BG);
}

static void drawTitleBar(const String& left, const String& right, uint16_t leftColor = C_TEXT) {
    tft.fillRect(0, TITLE_Y, SCR_W, TITLE_H + 1, C_BG);
    int rightW = right.length() ? tft.textWidth(right, FONT) + 8 : 0;
    drawText(fitText(left, SCR_W - 12 - rightW), 6, TITLE_Y + 1, leftColor);
    if (right.length()) drawText(right, SCR_W - 6, TITLE_Y + 1, C_DIM, TR_DATUM);
    tft.drawFastHLine(0, TITLE_Y + TITLE_H, SCR_W, C_BORDER);
}

static void drawMessage(const String& msg, uint16_t color = C_DIM) {
    tft.fillRect(0, ROW_Y, SCR_W, NAV_Y - ROW_Y, C_BG);
    drawText(msg, SCR_W / 2, ROW_Y + (NAV_Y - ROW_Y) / 2, color, MC_DATUM);
}

// =============================================================== data access

static int itemCount(int tab) {
    switch (tab) {
        case TAB_DEVICES: return devices.size();
        case TAB_CLIENTS: return clients.size();
        case TAB_FIREWALL: return 1 + firewall.forwards.size() + firewall.rules.size();  // 1 = summary row
        case TAB_ALERTS: return alerts.size();
    }
    return 0;
}

static bool fetchTab(int tab) {
    bool ok = false;
    switch (tab) {
        case TAB_DEVICES: ok = api->fetchDevices(devices); break;
        case TAB_CLIENTS: ok = api->fetchClients(clients); break;
        case TAB_FIREWALL: ok = api->fetchFirewall(firewall); break;
        case TAB_ALERTS: ok = api->fetchAlerts(alerts, alertsTotal); break;
    }
    PageState& s = ps[tab];
    s.lastFetch = millis();
    s.lastOk = ok;
    if (ok) s.loaded = true;
    int pages = max(1, (itemCount(tab) + ROWS - 1) / ROWS);
    if (s.page >= pages) s.page = pages - 1;
    if (s.selected >= itemCount(tab)) s.selected = -1;
    return ok;
}

// =============================================================== list view

static void titleForList(int tab) {
    switch (tab) {
        case TAB_DEVICES: {
            int online = 0;
            for (auto& d : devices) if (d.state == 1) online++;
            drawTitleBar("UniFi Devices", String(online) + "/" + String(devices.size()) + " online");
            break;
        }
        case TAB_CLIENTS: {
            int wired = 0;
            for (auto& c : clients) if (c.wired) wired++;
            drawTitleBar("Clients", String(clients.size()) + " total, " + String(wired) + " wired");
            break;
        }
        case TAB_FIREWALL:
            drawTitleBar("Firewall", String(firewall.total) + " rules, " + String(firewall.block) + " block");
            break;
        case TAB_ALERTS: {
            int active = 0;
            for (auto& a : alerts) if (a.active) active++;
            drawTitleBar("Alerts", active ? String(active) + " active" : String("no active alarms"),
                         active ? C_BAD : C_ACCENT);
            break;
        }
    }
}

static void drawDot(int x, int y, uint16_t color) {
    tft.fillCircle(x, y, 4, color);
}

static void drawRow(int tab, int idx, int y) {
    tft.fillRect(0, y, SCR_W, ROW_H, (idx % 2) ? C_ROW_ALT : C_BG);
    int ty = y + 2;
    int cy = y + ROW_H / 2;

    if (tab == TAB_DEVICES) {
        const UDevice& d = devices[idx];
        drawDot(10, cy, d.state == 1 ? C_GOOD : C_BAD);
        String right = d.model + "  " + String(d.clients);
        int rw = tft.textWidth(right, FONT);
        drawText(fitText(d.name, SCR_W - 22 - rw - 10), 20, ty, C_TEXT);
        drawText(right, SCR_W - 6, ty, C_DIM, TR_DATUM);
    } else if (tab == TAB_CLIENTS) {
        const UClient& c = clients[idx];
        drawDot(10, cy, c.wired ? C_ACCENT : signalColor(c.signal));
        int rw = tft.textWidth(c.ip, FONT);
        drawText(fitText(c.name, SCR_W - 22 - rw - 10), 20, ty, C_TEXT);
        drawText(c.ip, SCR_W - 6, ty, C_DIM, TR_DATUM);
    } else if (tab == TAB_FIREWALL) {
        if (idx == 0) {
            drawText("Summary", 20, ty, C_ACCENT);
            drawText(fmtCount(firewall.totalHits) + " hits", SCR_W - 6, ty, C_DIM, TR_DATUM);
            drawDot(10, cy, C_ACCENT);
            return;
        }
        int fi = idx - 1;
        if (fi < (int)firewall.forwards.size()) {
            const UPortForward& f = firewall.forwards[fi];
            tft.fillRoundRect(3, y + 3, 14, 14, 3, f.enabled ? C_ACCENT : C_PANEL);
            drawText("F", 10, cy, C_BG, MC_DATUM);
            String right = f.dstPort + ">" + f.fwdIp.substring(f.fwdIp.lastIndexOf('.'));
            int rw = tft.textWidth(right, FONT);
            drawText(fitText(f.name, SCR_W - 22 - rw - 10), 22, ty, C_TEXT);
            drawText(right, SCR_W - 6, ty, C_DIM, TR_DATUM);
            return;
        }
        const UFirewallRule& r = firewall.rules[fi - firewall.forwards.size()];
        bool allow = r.action.equalsIgnoreCase("ALLOW");
        tft.fillRoundRect(3, y + 3, 14, 14, 3, allow ? C_GOOD : C_BAD);
        drawText(allow ? "A" : "B", 10, cy, C_BG, MC_DATUM);
        String right = fmtCount(r.hits);
        int rw = tft.textWidth(right, FONT);
        drawText(fitText(r.name, SCR_W - 22 - rw - 10), 22, ty, r.enabled ? C_TEXT : C_DIM);
        drawText(right, SCR_W - 6, ty, C_DIM, TR_DATUM);
    } else if (tab == TAB_ALERTS) {
        const UAlert& a = alerts[idx];
        tft.fillRect(2, y + 3, 5, ROW_H - 6, severityColor(a.severity));
        String right = a.active ? String("ACTIVE") : fmtAgo(a.timestamp);
        int rw = tft.textWidth(right, FONT);
        drawText(fitText(a.title, SCR_W - 14 - rw - 10), 12, ty, C_TEXT);
        drawText(right, SCR_W - 6, ty, a.active ? C_BAD : C_DIM, TR_DATUM);
    }
}

static void drawListNav(int tab) {
    PageState& s = ps[tab];
    int pages = max(1, (itemCount(tab) + ROWS - 1) / ROWS);
    tft.fillRect(0, NAV_Y, SCR_W, UI_H - NAV_Y, C_BG);
    drawButton(BTN_PREV_X, BTN_PREV_W, "< Prev", s.page > 0);
    drawText(String(s.page + 1) + " / " + String(pages), PAGE_LABEL_X, NAV_Y + NAV_H / 2, C_DIM, MC_DATUM);
    drawButton(BTN_NEXT_X, BTN_NEXT_W, "Next >", s.page < pages - 1);
    drawButton(BTN_REFRESH_X, BTN_REFRESH_W, "Refresh", true);
}

static void drawList(int tab) {
    PageState& s = ps[tab];
    titleForList(tab);
    int n = itemCount(tab);
    if (!s.loaded) {
        drawMessage(s.lastOk ? "Loading..." : "Couldn't load data - tap Refresh", s.lastOk ? C_DIM : C_BAD);
    } else if (n == 0) {
        drawMessage(tab == TAB_ALERTS ? "No alerts - all clear" : "Nothing to show", C_GOOD);
    } else {
        for (int r = 0; r < ROWS; r++) {
            int idx = s.page * ROWS + r;
            int y = ROW_Y + r * ROW_H;
            if (idx < n) drawRow(tab, idx, y);
            else tft.fillRect(0, y, SCR_W, ROW_H, C_BG);
        }
        if (!s.lastOk) {
            // Keep showing the old data but flag the failed refresh
            drawText("!", SCR_W - 4, TITLE_Y + 2, C_BAD, TR_DATUM);
        }
    }
    drawListNav(tab);
}

// =============================================================== detail view

struct DLine {
    String label;   // empty -> full-width wrapped text
    String value;
    uint16_t color;
};

static void add(std::vector<DLine>& v, const String& label, const String& value, uint16_t color = C_TEXT) {
    if (value.length()) v.push_back({label, value, color});
}

static String detailTitle(int tab, int idx) {
    switch (tab) {
        case TAB_DEVICES: return devices[idx].name;
        case TAB_CLIENTS: return clients[idx].name;
        case TAB_FIREWALL:
            if (idx == 0) return "Firewall summary";
            if (idx - 1 < (int)firewall.forwards.size()) return "Port forward: " + firewall.forwards[idx - 1].name;
            return firewall.rules[idx - 1 - firewall.forwards.size()].name;
        case TAB_ALERTS: return alerts[idx].title;
    }
    return "";
}

static void buildDetail(int tab, int idx, std::vector<DLine>& v) {
    if (tab == TAB_DEVICES) {
        const UDevice& d = devices[idx];
        add(v, "Type", deviceTypeName(d.type));
        add(v, "Model", d.model);
        add(v, "Status", deviceStateName(d.state), d.state == 1 ? C_GOOD : C_BAD);
        add(v, "IP", d.ip);
        add(v, "MAC", d.mac);
        add(v, "Firmware", d.version + (d.upgradable ? "  (update available)" : ""), d.upgradable ? C_WARN : C_TEXT);
        if (d.uptime) add(v, "Uptime", fmtDuration(d.uptime));
        add(v, "Clients", String(d.clients));
        if (d.satisfaction >= 0) add(v, "Experience", String(d.satisfaction) + "%");
        if (d.cpu >= 0) add(v, "CPU", String(d.cpu, 1) + "%");
        if (d.mem >= 0) add(v, "Memory", String(d.mem, 1) + "%");
        if (d.temp > 0) add(v, "Temperature", String(d.temp, 1) + " C");
        if (d.uplinkName.length() || d.uplinkSpeed)
            add(v, "Uplink", (d.uplinkName.length() ? d.uplinkName : String("Wired")) +
                                 (d.uplinkSpeed ? " @ " + fmtSpeed(d.uplinkSpeed) : String("")));
        for (auto& r : d.radios) {
            add(v, "Radio " + r.band, (r.channel ? "ch " + String(r.channel) : String("off")) + ", " +
                                          String(r.clients) + " clients");
        }
        for (auto& p : d.ports) {
            String val = p.up ? (fmtSpeed(p.speed) + " up") : String("down");
            if (p.name.length()) val = p.name + " - " + val;
            add(v, "Port " + String(p.idx), val, p.up ? C_TEXT : C_DIM);
        }
    } else if (tab == TAB_CLIENTS) {
        const UClient& c = clients[idx];
        add(v, "IP", c.ip);
        add(v, "MAC", c.mac);
        if (c.hostname.length() && c.hostname != c.name) add(v, "Hostname", c.hostname);
        add(v, "Vendor", c.vendor);
        if (c.wired) {
            add(v, "Connection", "Wired" + (c.swPort ? " port " + String(c.swPort) : String("")));
        } else {
            add(v, "Connection", "WiFi " + c.essid);
            if (c.signal) add(v, "Signal", String(c.signal) + " dBm (" + signalWord(c.signal) + ")", signalColor(c.signal));
            if (c.channel) add(v, "Channel", String(c.channel) + (c.radio.length() ? " (" + c.radio + ")" : String("")));
        }
        add(v, "Via", c.uplink);
        add(v, "Network", c.network);
        if (c.satisfaction >= 0) add(v, "Experience", String(c.satisfaction) + "%");
        if (c.uptime) add(v, "Connected", fmtDuration(c.uptime));
        // UniFi counts bytes from the network's side: rx = sent by the client
        add(v, "Downloaded", fmtBytes(c.txBytes));
        add(v, "Uploaded", fmtBytes(c.rxBytes));
        if (c.guest) add(v, "Guest", "Yes", C_WARN);
    } else if (tab == TAB_FIREWALL) {
        if (idx == 0) {
            const UFirewallInfo& f = firewall;
            add(v, "Total rules", String(f.total));
            add(v, "Enabled", String(f.enabled));
            add(v, "Custom", String(f.custom));
            add(v, "Allow rules", String(f.allow), C_GOOD);
            add(v, "Block rules", String(f.block), C_BAD);
            add(v, "Total hits", fmtCount(f.totalHits));
            add(v, "Blocked hits", fmtCount(f.blockHits), C_BAD);
            add(v, "Port fwds", String(f.forwards.size()));
            return;
        }
        int fi = idx - 1;
        if (fi < (int)firewall.forwards.size()) {
            const UPortForward& f = firewall.forwards[fi];
            add(v, "Status", f.enabled ? "Enabled" : "Disabled", f.enabled ? C_GOOD : C_DIM);
            String proto = f.proto;
            proto.toUpperCase();
            add(v, "Protocol", proto);
            add(v, "WAN port", f.dstPort);
            add(v, "Forward to", f.fwdIp + ":" + f.fwdPort);
            return;
        }
        const UFirewallRule& r = firewall.rules[fi - firewall.forwards.size()];
        bool allow = r.action.equalsIgnoreCase("ALLOW");
        add(v, "Action", allow ? "Allow" : r.action, allow ? C_GOOD : C_BAD);
        add(v, "From zone", r.src);
        add(v, "To zone", r.dst);
        add(v, "Protocol", r.protocol);
        add(v, "Hits", fmtCount(r.hits) + " (" + String(r.hits, 0) + ")");
        if (r.lastHit > 0) add(v, "Last hit", fmtDateTime(r.lastHit));
        add(v, "Enabled", r.enabled ? "Yes" : "No", r.enabled ? C_GOOD : C_DIM);
        add(v, "Type", r.predefined ? "Built-in" : "Custom");
    } else if (tab == TAB_ALERTS) {
        const UAlert& a = alerts[idx];
        add(v, "Severity", severityName(a.severity), severityColor(a.severity));
        if (a.active) add(v, "Status", "Active", C_BAD);
        add(v, "Category", prettyCategory(a.category));
        add(v, "Time", fmtDateTime(a.timestamp));
        v.push_back({"", a.message, C_TEXT});
    }
}

// Word-wrap text to a pixel width
static void wrapText(const String& text, int maxW, std::vector<String>& out) {
    String line;
    int i = 0, n = text.length();
    while (i < n) {
        int sp = text.indexOf(' ', i);
        if (sp < 0) sp = n;
        String word = text.substring(i, sp);
        String candidate = line.length() ? line + " " + word : word;
        if (tft.textWidth(candidate, FONT) <= maxW) {
            line = candidate;
        } else {
            if (line.length()) out.push_back(line);
            // Hard-split words longer than a full line
            while (tft.textWidth(word, FONT) > maxW && word.length() > 1) {
                int cut = word.length();
                while (cut > 1 && tft.textWidth(word.substring(0, cut), FONT) > maxW) cut--;
                out.push_back(word.substring(0, cut));
                word = word.substring(cut);
            }
            line = word;
        }
        i = sp + 1;
    }
    if (line.length()) out.push_back(line);
}

struct VLine {
    String label, text;
    uint16_t color;
    bool inValueColumn;   // labelled value (or its wrapped continuation) vs. full-width text
};

static void layoutDetail(int tab, int idx, std::vector<VLine>& out) {
    std::vector<DLine> lines;
    buildDetail(tab, idx, lines);
    for (auto& l : lines) {
        std::vector<String> parts;
        if (l.label.length()) {
            wrapText(l.value, SCR_W - LABEL_W - 10, parts);
            for (size_t k = 0; k < parts.size(); k++)
                out.push_back({k == 0 ? l.label : String(""), parts[k], l.color, true});
        } else {
            wrapText(l.value, SCR_W - 12, parts);
            for (auto& p : parts) out.push_back({"", p, l.color, false});
        }
    }
}

static void drawDetail(int tab) {
    PageState& s = ps[tab];
    int idx = s.selected;
    drawTitleBar(detailTitle(tab, idx), "");

    std::vector<VLine> vl;
    layoutDetail(tab, idx, vl);
    s.detailPages = max(1, (int)((vl.size() + DETAIL_LINES - 1) / DETAIL_LINES));
    if (s.detailPage >= s.detailPages) s.detailPage = s.detailPages - 1;

    tft.fillRect(0, ROW_Y, SCR_W, NAV_Y - ROW_Y, C_BG);
    for (int r = 0; r < DETAIL_LINES; r++) {
        size_t i = s.detailPage * DETAIL_LINES + r;
        if (i >= vl.size()) break;
        int y = ROW_Y + 2 + r * DETAIL_LINE_H;
        const VLine& l = vl[i];
        if (l.label.length()) drawText(l.label, 6, y, C_DIM);
        drawText(l.text, l.inValueColumn ? LABEL_W : 6, y, l.color);
    }

    tft.fillRect(0, NAV_Y, SCR_W, UI_H - NAV_Y, C_BG);
    drawButton(4, BTN_BACK_W, "< Back", true);
    if (s.detailPages > 1) {
        drawButton(BTN_DPREV_X, BTN_DPREV_W, "Prev", s.detailPage > 0);
        drawText(String(s.detailPage + 1) + "/" + String(s.detailPages),
                 (BTN_DPREV_X + BTN_DPREV_W + BTN_REFRESH_X) / 2, NAV_Y + NAV_H / 2, C_DIM, MC_DATUM);
        drawButton(BTN_REFRESH_X, BTN_REFRESH_W, "More", s.detailPage < s.detailPages - 1);
    }
}

// =============================================================== public API

void pagesInit(UniFiAPI* a) {
    api = a;
}

static void drawPage(int tab) {
    if (ps[tab].selected >= 0) drawDetail(tab);
    else drawList(tab);
}

// Keep only the visible tab's list in RAM: each HTTPS fetch needs ~45 KB free for TLS
template <typename T> static void releaseVector(std::vector<T>& v) { std::vector<T>().swap(v); }

void pagesReleaseAll(int keepTab) {
    if (keepTab != TAB_DEVICES)  { releaseVector(devices); ps[TAB_DEVICES].loaded = false; }
    if (keepTab != TAB_CLIENTS)  { releaseVector(clients); ps[TAB_CLIENTS].loaded = false; }
    if (keepTab != TAB_FIREWALL) { releaseVector(firewall.rules); releaseVector(firewall.forwards); ps[TAB_FIREWALL].loaded = false; }
    if (keepTab != TAB_ALERTS)   { releaseVector(alerts); ps[TAB_ALERTS].loaded = false; }
}

void pagesShow(int tab) {
    if (tab <= TAB_OVERVIEW || tab > TAB_COUNT) return;
    PageState& s = ps[tab];
    pagesReleaseAll(tab);
    clearContent();
    bool stale = !s.loaded || millis() - s.lastFetch > REFRESH_MS[tab];
    if (stale) {
        s.selected = -1;
        s.lastOk = true;
        drawList(tab);                  // shows "Loading..." or the old list
        if (s.loaded) drawText("updating", SCR_W / 2, NAV_Y + NAV_H / 2, C_WARN, MC_DATUM);
        fetchTab(tab);
    }
    drawPage(tab);
}

void pagesTouch(int tab, int x, int y) {
    if (tab <= TAB_OVERVIEW || tab > TAB_COUNT) return;
    PageState& s = ps[tab];
    int n = itemCount(tab);

    if (s.selected >= 0) {
        // Detail view
        if (y >= NAV_Y - 4) {
            if (x < BTN_BACK_W + 14) {
                s.selected = -1;
                drawList(tab);
            } else if (s.detailPages > 1 && x >= BTN_DPREV_X - 8 && x < BTN_DPREV_X + BTN_DPREV_W + 8 && s.detailPage > 0) {
                s.detailPage--;
                drawDetail(tab);
            } else if (s.detailPages > 1 && x >= BTN_REFRESH_X - 10 && s.detailPage < s.detailPages - 1) {
                s.detailPage++;
                drawDetail(tab);
            }
        }
        return;
    }

    // List view
    if (y >= NAV_Y - 4) {
        int pages = max(1, (n + ROWS - 1) / ROWS);
        if (x < BTN_PREV_X + BTN_PREV_W + 8 && s.page > 0) {
            s.page--;
            drawList(tab);
        } else if (x >= BTN_NEXT_X - 8 && x < BTN_NEXT_X + BTN_NEXT_W + 8 && s.page < pages - 1) {
            s.page++;
            drawList(tab);
        } else if (x >= BTN_REFRESH_X - 8) {
            drawButton(BTN_REFRESH_X, BTN_REFRESH_W, "...", true);
            fetchTab(tab);
            drawList(tab);
        }
        return;
    }

    if (y >= ROW_Y && y < ROW_Y + ROWS * ROW_H && s.loaded) {
        int idx = s.page * ROWS + (y - ROW_Y) / ROW_H;
        if (idx < n) {
            s.selected = idx;
            s.detailPage = 0;
            // Brief highlight so the tap feels responsive
            tft.drawRect(0, ROW_Y + (idx - s.page * ROWS) * ROW_H, SCR_W, ROW_H, C_ACCENT);
            drawDetail(tab);
        }
    }
}

void pagesLoop(int tab) {
    if (tab <= TAB_OVERVIEW || tab > TAB_COUNT) return;
    PageState& s = ps[tab];
    // Auto-refresh only while looking at the list, so a detail view doesn't jump around
    if (s.selected < 0 && millis() - s.lastFetch > REFRESH_MS[tab]) {
        drawText("updating", PAGE_LABEL_X, NAV_Y + NAV_H / 2, C_WARN, MC_DATUM);
        fetchTab(tab);
        drawList(tab);
    }
}
