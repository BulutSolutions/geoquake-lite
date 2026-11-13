#include "quake_notifications.h"
#include "quake_seen_policy.h"
#include <esp_heap_caps.h>
#include <string.h>

static std::vector<QuakeNotification> notifications;  // the newest alerted earthquakes, newest first
static quake_seen_policy::Seen* s_seen = nullptr;     // ids already alerted (7.2 KB, in PSRAM)
static bool initialized = false;
static const int kMaxNotifications = 10;

static_assert(quake_seen_policy::kIdLen == sizeof(Earthquake::id), "the remembered ids hold a whole earthquake id");
static_assert(quake_seen_policy::kCapacity >= 2 * MAX_STORED_QUAKES, "an id still in the feed must not be given up");

// The memory of alerted ids. True when it was obtained by this call (it is empty then).
static bool obtain_seen() {
    if (s_seen) return false;
    s_seen = static_cast<quake_seen_policy::Seen*>(
        heap_caps_calloc(1, sizeof(quake_seen_policy::Seen), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_seen) s_seen = static_cast<quake_seen_policy::Seen*>(calloc(1, sizeof(quake_seen_policy::Seen)));
    return s_seen != nullptr;
}

void quake_notifs_init() {
    if (initialized) return;
    notifications.reserve(kMaxNotifications);
    obtain_seen();
    initialized = true;
}

// Returns how many earthquakes of this refresh are new: they pass the filter and were not alerted before. The list
// used to remember only its 10 entries, so with more than 10 earthquakes over the filter every refresh counted the
// rest as new again and the alert sound played each time.
int quake_notifs_add_from_list(const std::vector<Earthquake>& quakes, float min_mag) {
    quake_notifs_init();
    // No memory for the ids at start (it is asked for again on every refresh): no alert rather than one on every
    // refresh. The refresh that gets the memory only fills it: everything in the feed would count as new.
    const bool only_fill = obtain_seen();
    if (!s_seen) return 0;
    quake_seen_policy::begin_refresh(*s_seen);
    int added = 0;
    // The feed is newest first; walked from its oldest, the newest is inserted last and ends at the list's front.
    for (auto it = quakes.rbegin(); it != quakes.rend(); ++it) {
        const Earthquake& q = *it;
        if (!quake_seen_policy::note(*s_seen, q.id, q.mag, min_mag)) continue;

        QuakeNotification n = {};
        strncpy(n.id, q.id, sizeof(n.id) - 1);
        n.mag = q.mag;
        strncpy(n.place, q.place, sizeof(n.place) - 1);
        n.time = q.time;
        n.lat = q.lat;
        n.lon = q.lon;
        n.unread = true;

        notifications.insert(notifications.begin(), n);
        added++;
        if ((int)notifications.size() > kMaxNotifications) {
            notifications.pop_back();
        }
    }
    return only_fill ? 0 : added;
}

int quake_notifs_count() {
    return (int)notifications.size();
}

int quake_notifs_unread_count() {
    int count = 0;
    for (const auto& n : notifications) {
        if (n.unread) count++;
    }
    return count;
}

const QuakeNotification* quake_notifs_get(int index) {
    if (index < 0 || index >= (int)notifications.size()) return nullptr;
    return &notifications[index];
}

void quake_notifs_mark_all_read() {
    for (auto& n : notifications) {
        n.unread = false;
    }
}
