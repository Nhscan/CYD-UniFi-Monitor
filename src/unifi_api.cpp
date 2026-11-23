// UniFi OS login + overview numbers.
// Works with any UniFi OS console (UDM / UDM Pro / UCG / UXG / Cloud Key Gen2) using a
// local admin account. Detail lists live in unifi_lists.cpp.
#include <Arduino.h>
#include <HTTPClient.h>
#include "unifi_api.h"
#include "config.h"

static const char* LOGIN_PATH = "/api/auth/login";
static const char* SITE_V1 = "/proxy/network/api/s/default";

UniFiAPI::UniFiAPI() {
    client.setInsecure();   // consoles use self-signed certificates
}

void UniFiAPI::configure(const String& host, const String& user, const String& pass) {
    String h = host;
    h.trim();
    if (h.startsWith("https://")) h = h.substring(8);
    if (h.startsWith("http://")) h = h.substring(7);
    while (h.endsWith("/")) h.remove(h.length() - 1);
    baseUrl = "https://" + h;
    username = user;
    password = pass;
    authenticated = false;
    sessionCookie = "";
    csrfToken = "";
}

bool UniFiAPI::authenticate() {
    authenticated = false;
    if (baseUrl.isEmpty()) {
        lastError_ = "No UniFi host set";
        return false;
    }

    HTTPClient https;
    https.setTimeout(10000);
    if (!https.begin(client, baseUrl + LOGIN_PATH)) {
        lastError_ = "Can't reach console";
        return false;
    }
    https.addHeader("Content-Type", "application/json");
    // UniFi OS sends several Set-Cookie headers on login and HTTPClient's
    // collectHeaders() keeps only the last one, which loses TOKEN. Collect all.
    https.collectAllHeaders(true);

    JsonDocument body;
    body["username"] = username;
    body["password"] = password;
    body["remember"] = false;
    String request;
    serializeJson(body, request);

    int code = https.POST(request);
    if (code != HTTP_CODE_OK) {
        if (code == 401 || code == 400) lastError_ = "UniFi login rejected - check user/password";
        else if (code == 429) lastError_ = "UniFi login rate-limited - wait a minute";
        else if (code < 0) lastError_ = "Can't reach console at " + baseUrl.substring(8);
        else lastError_ = "UniFi login failed (HTTP " + String(code) + ")";
        Serial.println(lastError_);
        https.end();
        return false;
    }

    sessionCookie = "";
    for (int i = 0; i < https.headers() && sessionCookie.isEmpty(); i++) {
        if (!https.headerName(i).equalsIgnoreCase("Set-Cookie")) continue;
        String cookies = https.header(i);
        int start = cookies.indexOf("TOKEN=");
        if (start >= 0) {
            int end = cookies.indexOf(';', start);
            sessionCookie = cookies.substring(start, end < 0 ? cookies.length() : end);
        }
    }
    csrfToken = https.header("X-Csrf-Token");
    https.end();

    if (sessionCookie.isEmpty()) {
        lastError_ = "Login OK but no session cookie (not UniFi OS?)";
        Serial.println(lastError_);
        return false;
    }
    authenticated = true;
    lastAuthTime = millis();
    lastError_ = "";
    Serial.println("UniFi login OK");
    return true;
}

bool UniFiAPI::isConnected() {
    // Refresh the session every 20 minutes so it never expires mid-use
    if (authenticated && millis() - lastAuthTime > 20UL * 60 * 1000) {
        authenticate();
    }
    return authenticated;
}

static float numOrNeg(JsonVariantConst v) {
    if (v.isNull()) return -1;
    if (v.is<const char*>()) return atof(v.as<const char*>());
    return v.as<float>();
}

bool UniFiAPI::fetchOverview(NetworkStats& stats) {
    // /stat/health is small (~10 KB) and has everything the overview needs
    JsonDocument filter;
    JsonObject f = filter["data"][0].to<JsonObject>();
    f["subsystem"] = true;
    f["num_user"] = true;
    f["rx_bytes-r"] = true;
    f["tx_bytes-r"] = true;
    f["latency"] = true;
    f["uptime_stats"]["*"]["availability"] = true;
    f["uptime_stats"]["*"]["latency_average"] = true;

    JsonDocument doc;
    if (requestJson(String(SITE_V1) + "/stat/health", nullptr, filter, doc) != 200) return false;

    float experience = stats.experience;   // kept from fetchExperience()
    stats = NetworkStats();
    stats.experience = experience;
    int clients = 0;
    bool haveClients = false;
    float wwwLatency = -1;

    for (JsonObjectConst h : doc["data"].as<JsonArrayConst>()) {
        String sub = h["subsystem"] | "";
        if (sub == "wan") {
            stats.download_mbps = (h["rx_bytes-r"] | 0.0) * 8.0 / 1e6;
            stats.upload_mbps = (h["tx_bytes-r"] | 0.0) * 8.0 / 1e6;
            // uptime_stats is keyed by WAN name ("WAN", "WAN2"...); use the primary
            JsonObjectConst us = h["uptime_stats"].as<JsonObjectConst>();
            JsonObjectConst primary = us["WAN"].as<JsonObjectConst>();
            if (primary.isNull()) {
                for (JsonPairConst kv : us) { primary = kv.value().as<JsonObjectConst>(); break; }
            }
            if (!primary.isNull()) {
                stats.uptime_percent = numOrNeg(primary["availability"]);
                stats.ping_ms = numOrNeg(primary["latency_average"]);
            }
        } else if (sub == "wlan" || sub == "lan") {
            if (!h["num_user"].isNull()) {
                clients += h["num_user"].as<int>();
                haveClients = true;
            }
        } else if (sub == "www") {
            wwwLatency = numOrNeg(h["latency"]);
        }
    }
    if (haveClients) stats.clients = clients;
    if (stats.ping_ms < 0) stats.ping_ms = wwwLatency;
    stats.valid = true;

    #if DEBUG_API
    Serial.printf("Overview: down %.2f up %.2f Mbps, %d clients, ping %.1f, uptime %.1f%%\n",
                  stats.download_mbps, stats.upload_mbps, stats.clients, stats.ping_ms, stats.uptime_percent);
    #endif
    return true;
}

bool UniFiAPI::fetchExperience(float& experience) {
    // Average WiFi experience ("satisfaction") reported by the access points
    JsonDocument filter;
    filter["data"][0]["type"] = true;
    filter["data"][0]["state"] = true;
    filter["data"][0]["satisfaction"] = true;
    JsonDocument doc;
    if (requestJson(String(SITE_V1) + "/stat/device", nullptr, filter, doc) != 200) return false;

    int sum = 0, count = 0;
    for (JsonObjectConst d : doc["data"].as<JsonArrayConst>()) {
        int sat = d["satisfaction"] | -1;
        if ((d["state"] | 0) == 1 && sat >= 0) {
            sum += sat;
            count++;
        }
    }
    experience = count ? (float)sum / count : -1;
    return true;
}
