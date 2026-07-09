# Screen Debug Server

This module is a Wi-Fi LAN debug helper for viewing and driving the device screen from a browser.

Build switch:

`CONFIG_APP_DEBUG_SCREEN_SERVER_ENABLE`

When disabled, this folder is not compiled and no debug server task is started.

When enabled, browse to:

`http://<device-ip>:8080/`

The browser view follows the active LVGL viewport. On the P4 landscape build
the screen is exposed as `480 x 320`.

Routes:

- `/` browser debug page
- `/screen.bmp` current LVGL screen snapshot
- `/api/status` JSON status
- `/api/tap?x=<x>&y=<y>` dispatch a screen tap
- `/api/scroll?x=<x>&y=<y>&dx=<dx>&dy=<dy>` dispatch a screen scroll
