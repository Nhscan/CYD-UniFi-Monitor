#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>

// Everything the user configures, saved in the ESP32's flash (NVS)
struct Settings {
    String wifiSsid;
    String wifiPass;
    String unifiHost;
    String unifiUser;
    String unifiPass;
    String timezone = "EST5EDT,M3.2.0,M11.1.0";   // POSIX TZ string
    bool clock24h = false;
    bool rotate180 = false;
    bool invertColors = false;

    bool isComplete() const {
        return wifiSsid.length() && unifiHost.length() && unifiUser.length() && unifiPass.length();
    }
};

bool settingsLoad(Settings& s);          // false if nothing saved yet
void settingsSave(const Settings& s);
void settingsClearTouchCalibration();

// Touch calibration for boards whose touch shares the display bus (TFT_eSPI format)
bool settingsLoadTouchCal(uint16_t cal[5]);
void settingsSaveTouchCal(const uint16_t cal[5]);

// Blocking: starts the "UniFi-Monitor-XXXX" hotspot + setup web page and draws
// instructions on screen. Restarts the device after saving, or after the
// timeout when `timeoutMs` > 0. Never returns.
void runSetupPortal(const Settings& current, const String& reason, unsigned long timeoutMs);

#endif // SETTINGS_H
