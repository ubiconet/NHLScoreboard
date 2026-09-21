#pragma once

// Boot status page: shown for BOOT_SETUP_PAGE_MS after the splash while the
// network connects, the firmware-update check runs, and game data loads
// behind it on core 0. Redraws only when connectivity changes.
// deviceName ("MLB Scoreboard") comes from the sport branding.
void renderBootStatusPage(const char* deviceName);
