#ifndef CONFIG_H
#define CONFIG_H

#define FIRMWARE_VERSION "2.0.0"

#ifndef BOARD_NAME
#define BOARD_NAME "ESP32 CYD"
#endif

// Timing
#define WIFI_CONNECT_TIMEOUT_MS   20000     // give up and open setup after this
#define OVERVIEW_POLL_MS          2000      // throughput / ping / clients
#define EXPERIENCE_POLL_MS        30000     // AP satisfaction (larger request)
#define UNIFI_RETRY_MS            30000     // retry login after a failure
#define SETUP_PORTAL_TIMEOUT_MS   600000    // reboot + retry if setup is left open (10 min)

// Default NTP server for the clock
#define NTP_SERVER "pool.ntp.org"

// Set to 1 in platformio.ini to log every API call
#ifndef DEBUG_API
#define DEBUG_API 0
#endif

#endif // CONFIG_H
