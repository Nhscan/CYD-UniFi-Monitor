// Detail-list fetchers for the Devices / Clients / Firewall / Alerts tabs.
// Responses can be 100-200 KB, so they are stream-parsed through an ArduinoJson
// filter that keeps only the fields the UI shows.
#include <Arduino.h>
#include <HTTPClient.h>
#include <algorithm>
#include "unifi_api.h"
#include "config.h"

static const char* SITE_V1 = "/proxy/network/api/s/default";
static const char* SITE_V2 = "/proxy/network/v2/api/site/default";

// Some UniFi numbers arrive as strings ("26.5"); accept either form.
static float numOf(JsonVariantConst v, float def = -1) {
    if (v.isNull()) return def;
    if (v.is<const char*>()) return atof(v.as<const char*>());
    return v.as<float>();
}

static String strOf(JsonVariantConst v, const char* def = "") {
    if (v.isNull()) return String(def);
    if (v.is<const char*>()) return String(v.as<const char*>());
    return v.as<String>();
}

static bool lessNoCase(const String& a, const String& b) {
    return strcasecmp(a.c_str(), b.c_str()) < 0;
}

// Buffered reader for ArduinoJson. The TLS client's read() is non-blocking and the
// Stream-level timeout it inherits is only 1 s, which the UniFi console regularly
// exceeds mid-response on large lists. This waits up to timeoutMs for more data.
class TlsJsonReader {
public:
    TlsJsonReader(Client& c, unsigned long timeoutMs) : client_(c), timeoutMs_(timeoutMs) {}

    int read() {
        if (pos_ >= len_ && !fill()) return -1;
        return buf_[pos_++];
    }

    size_t readBytes(char* out, size_t n) {
        size_t got = 0;
        while (got < n) {
            if (pos_ >= len_ && !fill()) break;
            size_t k = std::min(n - got, (size_t)(len_ - pos_));
            memcpy(out + got, buf_ + pos_, k);
            pos_ += k;
            got += k;
        }
        return got;
    }

private:
    bool fill() {
        unsigned long start = millis();
        while (true) {
            int n = client_.read(buf_, sizeof(buf_));
            if (n > 0) {
                len_ = n;
                pos_ = 0;
                return true;
            }
            if (!client_.connected() && client_.available() <= 0) return false;
            if (millis() - start > timeoutMs_) return false;
            delay(2);
        }
    }

    Client& client_;
    unsigned long timeoutMs_;
    uint8_t buf_[1024];
    int len_ = 0, pos_ = 0;
};

int UniFiAPI::requestJson(const String& path, const char* postBody, JsonDocument& filter, JsonDocument& out) {
    for (int attempt = 0; attempt < 2; attempt++) {
        HTTPClient https;
        https.useHTTP10(true);          // no chunked encoding -> parse straight from the stream
        https.setTimeout(10000);
        if (!https.begin(client, baseUrl + path)) {
            Serial.println("requestJson: begin failed for " + path);
            return -1;
        }
        if (!sessionCookie.isEmpty()) https.addHeader("Cookie", sessionCookie);
        if (!csrfToken.isEmpty()) https.addHeader("X-Csrf-Token", csrfToken);
        https.addHeader("Accept", "application/json");

        int code;
        if (postBody) {
            https.addHeader("Content-Type", "application/json");
            code = https.POST((uint8_t*)postBody, strlen(postBody));
        } else {
            code = https.GET();
        }

        if (code == 401 && attempt == 0) {
            https.end();
            Serial.println("requestJson: 401, re-authenticating");
            if (!authenticate()) return 401;
            continue;
        }
        if (code != 200) {
            https.end();
            Serial.printf("requestJson: HTTP %d for %s\n", code, path.c_str());
            return code;
        }

        out.clear();
        unsigned long t0 = millis();
        TlsJsonReader reader(https.getStream(), 10000);
        DeserializationError err = deserializeJson(out, reader, DeserializationOption::Filter(filter));
        https.end();
        #if DEBUG_API
        Serial.printf("requestJson: %s parsed in %lu ms\n", path.c_str(), millis() - t0);
        #endif
        if (err) {
            Serial.printf("requestJson: JSON %s for %s\n", err.c_str(), path.c_str());
            return -2;
        }
        return 200;
    }
    return 401;
}

// ---------------------------------------------------------------- Devices

static String radioBand(const String& r) {
    if (r == "ng") return "2.4G";
    if (r == "na") return "5G";
    if (r == "6e") return "6G";
    return r;
}

static bool isGatewayType(const String& t) {
    return t == "uxg" || t == "ugw" || t == "udm" || t == "usg";
}

bool UniFiAPI::fetchDevices(std::vector<UDevice>& out) {
    JsonDocument filter;
    JsonObject f = filter["data"][0].to<JsonObject>();
    for (const char* k : {"name", "model", "type", "ip", "mac", "displayable_version", "version", "state",
                          "uptime", "num_sta", "satisfaction", "upgradable", "general_temperature"}) {
        f[k] = true;
    }
    f["system-stats"]["cpu"] = true;
    f["system-stats"]["mem"] = true;
    f["temperatures"][0]["value"] = true;
    f["uplink"]["uplink_device_name"] = true;
    f["uplink"]["speed"] = true;
    JsonObject pf = f["port_table"][0].to<JsonObject>();
    pf["port_idx"] = true; pf["name"] = true; pf["up"] = true; pf["speed"] = true;
    JsonObject rf = f["radio_table_stats"][0].to<JsonObject>();
    rf["radio"] = true; rf["channel"] = true; rf["num_sta"] = true;

    JsonDocument doc;
    if (requestJson(String(SITE_V1) + "/stat/device", nullptr, filter, doc) != 200) return false;

    out.clear();
    out.reserve(doc["data"].size());
    for (JsonObjectConst d : doc["data"].as<JsonArrayConst>()) {
        UDevice dev;
        dev.name = strOf(d["name"], "");
        dev.model = strOf(d["model"]);
        dev.type = strOf(d["type"]);
        if (dev.name.isEmpty()) dev.name = dev.model;
        dev.ip = strOf(d["ip"]);
        dev.mac = strOf(d["mac"]);
        dev.version = strOf(d["displayable_version"], strOf(d["version"]).c_str());
        dev.state = d["state"] | 0;
        dev.uptime = d["uptime"] | 0;
        dev.clients = d["num_sta"] | 0;
        dev.satisfaction = d["satisfaction"] | -1;
        dev.upgradable = d["upgradable"] | false;
        dev.cpu = numOf(d["system-stats"]["cpu"]);
        dev.mem = numOf(d["system-stats"]["mem"]);
        dev.temp = numOf(d["temperatures"][0]["value"], numOf(d["general_temperature"]));
        dev.uplinkName = strOf(d["uplink"]["uplink_device_name"]);
        dev.uplinkSpeed = d["uplink"]["speed"] | 0;
        for (JsonObjectConst p : d["port_table"].as<JsonArrayConst>()) {
            dev.ports.push_back({p["port_idx"] | 0, strOf(p["name"]), p["up"] | false, p["speed"] | 0});
        }
        for (JsonObjectConst r : d["radio_table_stats"].as<JsonArrayConst>()) {
            dev.radios.push_back({radioBand(strOf(r["radio"])), r["channel"] | 0, r["num_sta"] | 0});
        }
        out.push_back(dev);
    }
    std::sort(out.begin(), out.end(), [](const UDevice& a, const UDevice& b) {
        bool ga = isGatewayType(a.type), gb = isGatewayType(b.type);
        if (ga != gb) return ga;
        return lessNoCase(a.name, b.name);
    });
    Serial.printf("fetchDevices: %d devices, heap %u\n", (int)out.size(), ESP.getFreeHeap());
    return true;
}

// ---------------------------------------------------------------- Clients

bool UniFiAPI::fetchClients(std::vector<UClient>& out) {
    JsonDocument filter;
    JsonObject f = filter["data"][0].to<JsonObject>();
    for (const char* k : {"name", "hostname", "ip", "last_ip", "mac", "network", "essid", "last_uplink_name", "oui",
                          "radio_proto", "is_wired", "is_guest", "signal", "satisfaction", "channel", "sw_port",
                          "uptime", "rx_bytes", "tx_bytes", "wired-rx_bytes", "wired-tx_bytes"}) {
        f[k] = true;
    }

    JsonDocument doc;
    if (requestJson(String(SITE_V1) + "/stat/sta", nullptr, filter, doc) != 200) return false;

    out.clear();
    out.reserve(doc["data"].size());
    for (JsonObjectConst c : doc["data"].as<JsonArrayConst>()) {
        UClient cl;
        cl.mac = strOf(c["mac"]);
        cl.hostname = strOf(c["hostname"]);
        cl.vendor = strOf(c["oui"]);
        cl.name = strOf(c["name"]);
        if (cl.name.isEmpty()) cl.name = cl.hostname;
        if (cl.name.isEmpty()) {
            String tail = cl.mac.length() >= 5 ? cl.mac.substring(cl.mac.length() - 5) : cl.mac;
            cl.name = cl.vendor.isEmpty() ? cl.mac : cl.vendor + " " + tail;
        }
        cl.ip = strOf(c["ip"], strOf(c["last_ip"]).c_str());
        cl.network = strOf(c["network"]);
        cl.essid = strOf(c["essid"]);
        cl.uplink = strOf(c["last_uplink_name"]);
        cl.radio = strOf(c["radio_proto"]);
        cl.wired = c["is_wired"] | false;
        cl.guest = c["is_guest"] | false;
        cl.signal = c["signal"] | 0;
        cl.satisfaction = c["satisfaction"] | -1;
        cl.channel = c["channel"] | 0;
        cl.swPort = c["sw_port"] | 0;
        cl.uptime = c["uptime"] | 0;
        if (cl.wired) {
            cl.rxBytes = c["wired-rx_bytes"] | 0.0;
            cl.txBytes = c["wired-tx_bytes"] | 0.0;
        } else {
            cl.rxBytes = c["rx_bytes"] | 0.0;
            cl.txBytes = c["tx_bytes"] | 0.0;
        }
        out.push_back(cl);
    }
    std::sort(out.begin(), out.end(), [](const UClient& a, const UClient& b) { return lessNoCase(a.name, b.name); });
    Serial.printf("fetchClients: %d clients, heap %u\n", (int)out.size(), ESP.getFreeHeap());
    return true;
}

// ---------------------------------------------------------------- Firewall

bool UniFiAPI::fetchFirewall(UFirewallInfo& out) {
    // Zone id -> name, so rules can show "Internal -> External"
    std::vector<std::pair<String, String>> zones;
    {
        JsonDocument filter;
        filter[0]["_id"] = true;
        filter[0]["name"] = true;
        JsonDocument doc;
        if (requestJson(String(SITE_V2) + "/firewall/zone", nullptr, filter, doc) == 200) {
            for (JsonObjectConst z : doc.as<JsonArrayConst>()) {
                zones.push_back({strOf(z["_id"]), strOf(z["name"])});
            }
        }
    }
    auto zoneName = [&](const String& id) -> String {
        for (auto& z : zones) if (z.first == id) return z.second;
        return id.isEmpty() ? String("Any") : String("?");
    };

    JsonDocument filter;
    JsonObject f = filter[0].to<JsonObject>();
    for (const char* k : {"name", "action", "enabled", "predefined", "hits", "last_hit", "protocol"}) f[k] = true;
    f["source"]["zone_id"] = true;
    f["destination"]["zone_id"] = true;

    JsonDocument doc;
    if (requestJson(String(SITE_V2) + "/firewall-policies", nullptr, filter, doc) != 200) return false;

    out = UFirewallInfo();
    for (JsonObjectConst p : doc.as<JsonArrayConst>()) {
        UFirewallRule r;
        r.name = strOf(p["name"], "(unnamed)");
        r.action = strOf(p["action"]);
        r.protocol = strOf(p["protocol"], "all");
        r.enabled = p["enabled"] | false;
        r.predefined = p["predefined"] | false;
        r.hits = p["hits"] | 0.0;
        r.lastHit = p["last_hit"] | 0.0;
        r.src = zoneName(strOf(p["source"]["zone_id"]));
        r.dst = zoneName(strOf(p["destination"]["zone_id"]));

        out.total++;
        if (r.enabled) out.enabled++;
        if (!r.predefined) out.custom++;
        out.totalHits += r.hits;
        if (r.action.equalsIgnoreCase("ALLOW")) out.allow++;
        else { out.block++; out.blockHits += r.hits; }
        out.rules.push_back(r);
    }
    std::sort(out.rules.begin(), out.rules.end(), [](const UFirewallRule& a, const UFirewallRule& b) { return a.hits > b.hits; });

    // Port forwards (small list)
    {
        JsonDocument pfFilter;
        JsonObject pf = pfFilter["data"][0].to<JsonObject>();
        for (const char* k : {"name", "proto", "dst_port", "fwd", "fwd_port", "enabled"}) pf[k] = true;
        JsonDocument pfDoc;
        if (requestJson(String(SITE_V1) + "/rest/portforward", nullptr, pfFilter, pfDoc) == 200) {
            for (JsonObjectConst p : pfDoc["data"].as<JsonArrayConst>()) {
                out.forwards.push_back({strOf(p["name"]), strOf(p["proto"]), strOf(p["dst_port"]),
                                        strOf(p["fwd"]), strOf(p["fwd_port"]), p["enabled"] | false});
            }
        }
    }
    Serial.printf("fetchFirewall: %d rules, %d forwards, heap %u\n", out.total, (int)out.forwards.size(), ESP.getFreeHeap());
    return true;
}

// ---------------------------------------------------------------- Alerts

// Replace "{CLIENT}" style placeholders with parameters.CLIENT.name
static String fillTemplate(const String& tmpl, JsonObjectConst params) {
    String out;
    out.reserve(tmpl.length() + 32);
    int i = 0;
    while (i < (int)tmpl.length()) {
        char ch = tmpl[i];
        if (ch == '{') {
            int end = tmpl.indexOf('}', i);
            if (end > i) {
                String key = tmpl.substring(i + 1, end);
                JsonVariantConst v = params[key]["name"];
                if (!v.isNull()) out += strOf(v);
                else out += key;
                i = end + 1;
                continue;
            }
        }
        out += ch;
        i++;
    }
    return out;
}

static void buildAlertFilter(JsonDocument& filter) {
    JsonObject f = filter["data"][0].to<JsonObject>();
    for (const char* k : {"severity", "title_raw", "message_raw", "timestamp", "category", "status"}) f[k] = true;
    f["parameters"]["*"]["name"] = true;
    filter["total_element_count"] = true;
}

static void appendAlerts(JsonDocument& doc, std::vector<UAlert>& out, bool active) {
    for (JsonObjectConst a : doc["data"].as<JsonArrayConst>()) {
        // The device-alert feed also returns old alarms marked STALED/resolved; only NEW ones are open
        if (active && strOf(a["status"]) != "NEW") continue;
        UAlert al;
        al.severity = strOf(a["severity"], "LOW");
        al.title = strOf(a["title_raw"], "Alert");
        al.message = fillTemplate(strOf(a["message_raw"]), a["parameters"].as<JsonObjectConst>());
        al.category = strOf(a["category"]);
        al.timestamp = a["timestamp"] | 0.0;
        al.active = active;
        out.push_back(al);
    }
}

bool UniFiAPI::fetchAlerts(std::vector<UAlert>& out, int& totalMatching) {
    out.clear();
    totalMatching = 0;

    // Open device alarms (e.g. rogue AP detected) first
    {
        JsonDocument filter;
        buildAlertFilter(filter);
        JsonDocument doc;
        if (requestJson(String(SITE_V2) + "/system-log/device-alert", "{\"pageSize\":10,\"pageNumber\":0}", filter, doc) == 200) {
            appendAlerts(doc, out, true);
        }
    }

    // Recent important events (medium and above)
    JsonDocument filter;
    buildAlertFilter(filter);
    JsonDocument doc;
    int code = requestJson(String(SITE_V2) + "/system-log/all",
                           "{\"pageSize\":25,\"pageNumber\":0,\"severities\":[\"MEDIUM\",\"HIGH\",\"VERY_HIGH\"]}",
                           filter, doc);
    if (code != 200) return !out.empty();
    totalMatching = doc["total_element_count"] | 0;
    appendAlerts(doc, out, false);
    Serial.printf("fetchAlerts: %d shown (%d total), heap %u\n", (int)out.size(), totalMatching, ESP.getFreeHeap());
    return true;
}
