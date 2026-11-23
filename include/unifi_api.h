#ifndef UNIFI_API_H
#define UNIFI_API_H

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <vector>

// ---- Overview numbers (all straight from the UniFi controller) ----
struct NetworkStats {
    bool valid = false;
    float download_mbps = 0;     // WAN receive rate
    float upload_mbps = 0;       // WAN transmit rate
    int clients = -1;            // wired + wireless clients (-1 = unknown)
    float ping_ms = -1;          // WAN latency (-1 = unknown)
    float uptime_percent = -1;   // WAN availability (-1 = unknown)
    float experience = -1;       // average access-point satisfaction (-1 = unknown)
};

// ---- Detail data for the Devices / Clients / Firewall / Alerts tabs ----

struct UDevicePort {
    int idx;
    String name;
    bool up;
    int speed;          // Mbps
};

struct URadio {
    String band;        // "2.4G", "5G", "6G"
    int channel;
    int clients;
};

struct UDevice {
    String name, model, type, ip, mac, version, uplinkName;
    int state = 0;           // 1 = online
    uint32_t uptime = 0;     // seconds
    int clients = 0;
    int satisfaction = -1;   // -1 = unknown
    float cpu = -1, mem = -1, temp = -1;
    bool upgradable = false;
    int uplinkSpeed = 0;
    std::vector<UDevicePort> ports;
    std::vector<URadio> radios;
};

struct UClient {
    String name, hostname, ip, mac, network, essid, uplink, vendor, radio;
    bool wired = false, guest = false;
    int signal = 0;          // dBm (wireless only)
    int satisfaction = -1;
    int channel = 0;
    int swPort = 0;
    uint32_t uptime = 0;
    double rxBytes = 0, txBytes = 0;
};

struct UFirewallRule {
    String name, action, src, dst, protocol;
    double hits = 0;
    double lastHit = 0;      // epoch ms
    bool enabled = false, predefined = false;
};

struct UPortForward {
    String name, proto, dstPort, fwdIp, fwdPort;
    bool enabled = false;
};

struct UFirewallInfo {
    int total = 0, allow = 0, block = 0, enabled = 0, custom = 0;
    double totalHits = 0, blockHits = 0;
    std::vector<UFirewallRule> rules;     // sorted by hits, most first
    std::vector<UPortForward> forwards;
};

struct UAlert {
    String severity, title, message, category;
    double timestamp = 0;    // epoch ms
    bool active = false;     // from the device-alert feed (still open)
};

class UniFiAPI {
public:
    UniFiAPI();

    // host = IP or hostname of the UniFi OS console (UDM, UCG, Cloud Key G2, UXG...)
    void configure(const String& host, const String& user, const String& pass);
    bool authenticate();
    bool isConnected();
    const String& lastError() const { return lastError_; }

    // Overview numbers. Experience needs a bigger request, so refresh it less often.
    bool fetchOverview(NetworkStats& stats);
    bool fetchExperience(float& experience);

    // Detail lists (return false on network/API failure)
    bool fetchDevices(std::vector<UDevice>& out);
    bool fetchClients(std::vector<UClient>& out);
    bool fetchFirewall(UFirewallInfo& out);
    bool fetchAlerts(std::vector<UAlert>& out, int& totalMatching);

private:
    // Authenticated GET/POST that stream-parses the response through a filter
    // (keeps memory low for large responses). Re-authenticates once on 401.
    int requestJson(const String& path, const char* postBody, JsonDocument& filter, JsonDocument& out);

    WiFiClientSecure client;
    String baseUrl, username, password;
    String sessionCookie, csrfToken;
    String lastError_;
    bool authenticated = false;
    unsigned long lastAuthTime = 0;
};

#endif // UNIFI_API_H
