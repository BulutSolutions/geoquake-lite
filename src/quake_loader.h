#pragma once
#include <Arduino.h>
#include <atomic>
#include <vector>
#include "quake_types.h"  // Earthquake + MAX_STORED_QUAKES (also used by the native parser)

// Render-path record: only what the map dot renderer reads, 16 bytes, same
// index order as the Earthquake array. World coordinates are precomputed at
// ingestion (map_projection.h world_from_latlon), so per-frame projection is
// integer-only and never touches the 192-byte cold records.
struct HotQuake {
    uint32_t wx;      // Web Mercator world x, Q32
    uint32_t wy;      // Web Mercator world y, Q31
    float mag;
    uint32_t time_s;  // epoch seconds (the feed reports milliseconds)
};
static_assert(sizeof(HotQuake) == 16, "HotQuake must stay a 16-byte POD");

// Fetch result codes for error handling
enum QuakeFetchResult {
    FETCH_SUCCESS,
    FETCH_HTTP_ERROR,
    FETCH_PARSE_ERROR,
    FETCH_WIFI_ERROR,
    FETCH_TIMEOUT
};

// --- PUBLIC API ---
void init_quake_loader();
void fetch_quakes();
QuakeFetchResult fetch_quakes_with_result();
QuakeFetchResult quake_last_fetch_result();  // result of the last fetch (FETCH_SUCCESS before the first one)
// Wi-Fi did not connect, so no fetch ran: the status bar must show Offline, not the old "13/24h" as if current.
void quake_note_connect_failed();
void quake_dispatch_pending_audio();

// Zero-allocation, thread-safe direct read access for hot paths (e.g. Map Renderer)
size_t quake_get_count();
bool quake_read_item(size_t index, Earthquake& out);
bool quake_lock_read(const Earthquake** out_ptr, size_t* out_count);
void quake_unlock_read();

// Render path: copy the hot records into caller-owned storage if they changed
// since *version. Holds quake_mutex only for the copy (<= 1.6 KB), so drawing
// never blocks the fetch task. Returns false if the lock was busy (output and
// *version untouched; keep drawing the previous snapshot).
bool quake_snapshot_hot(HotQuake* out, size_t cap, size_t* out_count, uint32_t* version);

// Backward-compatible API
const std::vector<Earthquake>& get_quakes_list();
bool quake_copy_list(std::vector<Earthquake>& out);

// Load dummy test earthquakes (for coordinate alignment testing)
void load_test_earthquakes();

// Status flags
extern std::atomic<bool> is_fetching_quakes;
extern std::atomic<bool> quake_data_ready;  // one-shot "new data" event (exchange(false) consumes it)
extern unsigned long last_sync_time_ms;  // Timestamp of last successful fetch
extern time_t last_sync_epoch;           // Unix time of last successful fetch

// Abort/Cleanup
void abort_quake_fetch();
