#pragma once

#include "quake_loader.h"
#include <vector>

struct QuakeNotification {
    char id[32];
    float mag;
    char place[96];
    long long time;
    float lat;
    float lon;
    bool unread;
};

void quake_notifs_init();
int quake_notifs_add_from_list(const std::vector<Earthquake>& quakes, float min_mag);
int quake_notifs_count();
int quake_notifs_unread_count();
const QuakeNotification* quake_notifs_get(int index);
void quake_notifs_mark_all_read();
