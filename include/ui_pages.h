#ifndef UI_PAGES_H
#define UI_PAGES_H

#include "unifi_api.h"

// Tab numbers (1-based, matches main.cpp currentTab)
enum {
    TAB_OVERVIEW = 1,
    TAB_DEVICES,
    TAB_CLIENTS,
    TAB_FIREWALL,
    TAB_ALERTS,
    TAB_COUNT = 5
};

extern const char* const TAB_NAMES[TAB_COUNT];

void pagesInit(UniFiAPI* api);
void pagesShow(int tab);                 // draw the page (fetches data if stale)
void pagesTouch(int tab, int x, int y);  // touch inside the content area
void pagesLoop(int tab);                 // periodic auto-refresh
void pagesReleaseAll(int keepTab);       // free list data except keepTab (0 = all)

#endif // UI_PAGES_H
