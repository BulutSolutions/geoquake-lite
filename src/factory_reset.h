#pragma once

// GeoQuake Lite factory reset: clears every gql_* namespace (settings, time zone,
// theme, auto-dim schedule, orientation) and the saved
// Wi-Fi network. The map assets are kept. Returns true when every namespace was
// cleared. Synchronous NVS work only; the System Menu calls it on the LVGL task
// (from BOOT or Settings > Factory Reset), then restarts.
bool geoquake_factory_reset();
