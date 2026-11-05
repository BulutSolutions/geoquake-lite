#include "quake_notifications.h"
#include <string.h>

static std::vector<QuakeNotification> notifications;
static bool initialized = false;
static const int kMaxNotifications = 10;

void quake_notifs_init() {
    if (initialized) return;
    notifications.reserve(kMaxNotifications);
    initialized = true;
}

static bool has_id(const char* id) {
    if (!id || !id[0]) return false;
    for (const auto& n : notifications) {
        if (strncmp(n.id, id, sizeof(n.id)) == 0) return true;
    }
    return false;
}

int quake_notifs_add_from_list(const std::vector<Earthquake>& quakes, float min_mag) {
    quake_notifs_init();
    int added = 0;
    for (const auto& q : quakes) {
        if (q.mag < min_mag) continue;
        if (has_id(q.id)) continue;

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
    return added;
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
