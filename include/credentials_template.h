// OPTIONAL developer shortcut - most people never need this file.
//
// Normally you enter WiFi and UniFi details on the device's setup page
// (it opens automatically the first time it boots). If you build from source
// and want to skip that step, copy this file to credentials.h (which is
// git-ignored), fill it in, and build with:  -D USE_CREDENTIALS_H
// The values are copied into the device's saved settings on first boot.
#pragma once

#define WIFI_SSID      "your-wifi-name"
#define WIFI_PASSWORD  "your-wifi-password"
#define UNIFI_HOST     "192.168.1.1"
#define UNIFI_USERNAME "local-unifi-user"
#define UNIFI_PASSWORD "local-unifi-password"
