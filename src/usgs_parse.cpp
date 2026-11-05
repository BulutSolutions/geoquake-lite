#include "usgs_parse.h"

#include <stdlib.h>
#include <string.h>

namespace {

// A feature object is {type, id, properties{mag,place,time,...}, geometry{...}}.
constexpr uint8_t kNestingLimit = 8;
constexpr size_t kMaxFeatureBytes = 16u * 1024u;  // raw JSON of ONE feature object
constexpr size_t kMaxDocBytes = 16u * 1024u;      // JsonDocument built from that object
constexpr size_t kMaxOverflowSkips = 8;           // this many over-budget features -> TooLarge
constexpr size_t kMaxIdBytes = 64;                // sane raw lengths; longer = bogus record
constexpr size_t kMaxPlaceBytes = 256;

// Byte-budgeted reader with a one-byte pushback (so the parser can peek at the
// '{' / separator without losing it).
class CountingReader {
public:
    CountingReader(UsgsReader& in, size_t max_bytes) : in_(in), max_bytes_(max_bytes) {}

    int read() {
        if (push_ >= 0) {
            const int c = push_;
            push_ = -1;
            return c;
        }
        if (count_ >= max_bytes_) {
            too_large_ = true;
            return -1;
        }
        const int c = in_.read();
        if (c < 0) return -1;
        ++count_;
        return c;
    }

    size_t readBytes(char* buffer, size_t length) {
        if (length == 0) return 0;
        size_t n = 0;
        if (push_ >= 0) {
            buffer[n++] = static_cast<char>(push_);
            push_ = -1;
            if (n == length) return n;
        }
        size_t want = length - n;
        if (count_ + want > max_bytes_) {
            want = max_bytes_ - count_;  // count_ <= max_bytes_ always
            too_large_ = true;
        }
        if (want > 0) {
            const size_t got = in_.readBytes(buffer + n, want);
            count_ += got;
            n += got;
        }
        return n;
    }

    void unread(int c) { push_ = c; }
    bool too_large() const { return too_large_; }

private:
    UsgsReader& in_;
    const size_t max_bytes_;
    size_t count_ = 0;
    int push_ = -1;
    bool too_large_ = false;
};

bool is_space(int c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

int skip_ws(CountingReader& reader) {
    int c;
    while ((c = reader.read()) >= 0) {
        if (!is_space(c)) return c;
    }
    return -1;
}

// Why the byte stream ended: the reader hit its total deadline, the byte
// budget ran out, or it is a real end of file.
UsgsParseStatus eof_status(const CountingReader& reader, const UsgsReader& source, UsgsParseStatus eof_kind) {
    if (reader.too_large()) return UsgsParseStatus::TooLarge;
    if (source.timed_out()) return UsgsParseStatus::Timeout;
    return eof_kind;
}

// Replays a pushed-back byte first.
int next_byte(CountingReader& reader, int& pending) {
    if (pending >= 0) {
        const int c = pending;
        pending = -1;
        return c;
    }
    return reader.read();
}

void append_char(char* buf, size_t cap, size_t& len, bool& overflow, char c) {
    if (len < cap) {
        buf[len++] = c;
    } else {
        overflow = true;
    }
}

// Reads the next non-space byte, pushing it back when it is not ':'.
// Returns the byte after the ':' (which may be -1 at EOF).
int read_after_colon(CountingReader& reader, int& pending) {
    int c;
    do {
        c = next_byte(reader, pending);
    } while (is_space(c));
    if (c != ':') {
        pending = c;  // not a member key after all; keep scanning from here
        return -2;
    }
    do {
        c = next_byte(reader, pending);
    } while (is_space(c));
    return c;
}

// Finds the ROOT-level "features" member followed by ':' and '['. Tracks JSON
// strings (quotes, backslash escapes) and nesting, so "features" inside a
// string value or a nested object never matches.
bool find_features_array(CountingReader& reader) {
    static constexpr char kKey[] = "features";
    constexpr size_t kKeyLen = sizeof(kKey) - 1;

    char key[16];
    size_t key_len = 0;
    bool key_overflow = false;
    bool key_candidate = false;  // the current string sits at the root object level
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    int pending = -1;

    for (;;) {
        const int c = next_byte(reader, pending);
        if (c < 0) return false;

        if (in_string) {
            if (escaped) {
                escaped = false;
                if (key_candidate) append_char(key, sizeof(key), key_len, key_overflow, static_cast<char>(c));
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
                if (key_candidate && !key_overflow && key_len == kKeyLen &&
                    memcmp(key, kKey, kKeyLen) == 0) {
                    const int after = read_after_colon(reader, pending);
                    if (after == '[') return true;
                    if (after == -2) {  // not a member key: pending holds the byte to rescan
                        key_candidate = false;
                        continue;
                    }
                    if (after < 0) return false;  // EOF
                    pending = after;  // a non-array value: rescan it so nesting stays correct
                    key_candidate = false;
                }
            } else if (key_candidate) {
                append_char(key, sizeof(key), key_len, key_overflow, static_cast<char>(c));
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
            escaped = false;
            key_candidate = (depth == 1);
            key_len = 0;
            key_overflow = false;
        } else if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            if (depth > 0) --depth;
        }
    }
}

// Reads ONE '{...}' object (the leading '{' is already consumed) into a bounded
// buffer. On over-budget the whole object is still consumed, so the caller can
// skip it and continue with the next feature.
struct ObjectRead {
    size_t length = 0;
    bool complete = false;
    bool over_budget = false;
};

ObjectRead read_object(CountingReader& reader, uint8_t* buffer, size_t cap) {
    ObjectRead out;
    if (cap == 0) return out;

    buffer[out.length++] = '{';
    int depth = 1;
    bool in_string = false;
    bool escaped = false;

    while (depth > 0) {
        const int c = reader.read();
        if (c < 0) return out;  // truncated stream
        if (!out.over_budget) {
            if (out.length < cap) {
                buffer[out.length++] = static_cast<uint8_t>(c);
            } else {
                out.over_budget = true;
            }
        }
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
        } else if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            --depth;
        }
    }
    out.complete = true;
    return out;
}

class MallocAllocator : public ArduinoJson::Allocator {
public:
    void* allocate(size_t size) override { return malloc(size); }
    void deallocate(void* ptr) override { free(ptr); }
    void* reallocate(void* ptr, size_t new_size) override { return realloc(ptr, new_size); }
    static ArduinoJson::Allocator* instance() {
        static MallocAllocator alloc;
        return &alloc;
    }
};

// Forwards to `base` with a hard per-document byte cap: past it every
// allocation returns nullptr, so an oversized feature cannot grow without
// bound. Every block carries its size, so usage survives deallocate/reallocate.
class CappedAllocator : public ArduinoJson::Allocator {
public:
    CappedAllocator(ArduinoJson::Allocator* base, size_t cap) : base_(base), cap_(cap) {}

    void* allocate(size_t size) override {
        if (size > cap_ || used_ + size + sizeof(Block) > cap_) {
            overflow_ = true;
            return nullptr;
        }
        Block* block = static_cast<Block*>(base_->allocate(size + sizeof(Block)));
        if (block == nullptr) return nullptr;
        block->size = size;
        used_ += size + sizeof(Block);
        return block + 1;
    }

    void deallocate(void* ptr) override {
        if (ptr == nullptr) return;
        Block* block = static_cast<Block*>(ptr) - 1;
        used_ -= static_cast<size_t>(block->size) + sizeof(Block);
        base_->deallocate(block);
    }

    void* reallocate(void* ptr, size_t new_size) override {
        if (ptr == nullptr) return allocate(new_size);
        Block* block = static_cast<Block*>(ptr) - 1;
        const size_t old_size = static_cast<size_t>(block->size);
        if (new_size > old_size && used_ + (new_size - old_size) > cap_) {
            overflow_ = true;
            return nullptr;
        }
        Block* moved = static_cast<Block*>(base_->reallocate(block, new_size + sizeof(Block)));
        if (moved == nullptr) return nullptr;
        moved->size = new_size;
        used_ = used_ - old_size + new_size;
        return moved + 1;
    }

    bool overflowed() const { return overflow_; }
    void clear_overflow() { overflow_ = false; }

private:
    struct Block {
        uint64_t size;  // keeps the payload 8-byte aligned on every target
    };

    ArduinoJson::Allocator* base_;
    const size_t cap_;
    size_t used_ = 0;
    bool overflow_ = false;
};

void copy_field(char* dst, size_t cap, const char* src) {
    if (cap == 0) return;
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// Store the record; when the array is full the oldest record makes room for a
// newer one.
void store_record(Earthquake* out, size_t cap, size_t& count, const Earthquake& eq) {
    if (cap == 0) return;
    if (count < cap) {
        out[count++] = eq;
        return;
    }
    size_t oldest = 0;
    for (size_t i = 1; i < count; ++i) {
        if (out[i].time < out[oldest].time) oldest = i;
    }
    if (eq.time > out[oldest].time) out[oldest] = eq;
}

void sort_newest_first(Earthquake* out, size_t count) {
    for (size_t i = 1; i < count; ++i) {
        const Earthquake key = out[i];
        size_t j = i;
        while (j > 0 && out[j - 1].time < key.time) {
            out[j] = out[j - 1];
            --j;
        }
        out[j] = key;
    }
}

// Fills eq from a filtered feature object; false when the feature is unusable.
bool read_feature(JsonObject feature, Earthquake& eq, long long now_ms) {
    if (feature.isNull()) return false;

    JsonObject props = feature["properties"].as<JsonObject>();
    if (props.isNull()) return false;

    JsonVariant mag_v = props["mag"];
    if (!mag_v.is<float>()) return false;  // null/absent mag; a real 0.0 is kept

    JsonVariant time_v = props["time"];
    if (!time_v.is<long long>()) return false;
    const long long time_ms = time_v.as<long long>();
    if (time_ms <= 0) return false;
    // A time far in the future is a bogus record (clock is known when now_ms is set).
    if (now_ms > 0 && time_ms > now_ms + 24LL * 60LL * 60LL * 1000LL) return false;

    JsonArray coords = feature["geometry"]["coordinates"].as<JsonArray>();
    if (coords.isNull() || coords.size() < 2) return false;
    JsonVariant lon_v = coords[0];
    JsonVariant lat_v = coords[1];
    if (!lon_v.is<float>() || !lat_v.is<float>()) return false;
    const float lon = lon_v.as<float>();
    const float lat = lat_v.as<float>();
    if (!(lon >= -180.0f && lon <= 180.0f)) return false;
    if (!(lat >= -90.0f && lat <= 90.0f)) return false;

    // Bounded strings only: absurd ids/places are bogus records, not data.
    const char* id = feature["id"].as<const char*>();
    const char* place = props["place"].as<const char*>();
    if (id != nullptr && strlen(id) > kMaxIdBytes) return false;
    if (place != nullptr && strlen(place) > kMaxPlaceBytes) return false;
    if (!place || place[0] == '\0') place = "Unknown location";

    JsonVariant depth_v = coords[2];
    const float depth = depth_v.is<float>() ? depth_v.as<float>() : 0.0f;

    memset(&eq, 0, sizeof(Earthquake));
    copy_field(eq.id, sizeof(eq.id), id);
    copy_field(eq.place, sizeof(eq.place), place);
    eq.mag = mag_v.as<float>();
    eq.time = time_ms;
    eq.lat = lat;
    eq.lon = lon;
    eq.depth = depth;
    return true;
}

}  // namespace

UsgsParseResult usgs_parse_features(UsgsReader& reader_impl, Earthquake* out, size_t cap,
                                    const UsgsParseLimits& limits, ArduinoJson::Allocator* allocator) {
    UsgsParseResult result;
    CountingReader reader(reader_impl, limits.max_bytes);

    if (!find_features_array(reader)) {
        result.status = eof_status(reader, reader_impl, UsgsParseStatus::NoFeatures);
        return result;
    }

    int c = skip_ws(reader);
    if (c < 0) {
        result.status = eof_status(reader, reader_impl, UsgsParseStatus::Truncated);
        return result;
    }

    JsonDocument filter;
    {
        JsonObject f = filter.to<JsonObject>();
        f["id"] = true;
        JsonObject fp = f["properties"].to<JsonObject>();
        fp["mag"] = true;
        fp["place"] = true;
        fp["time"] = true;
        JsonObject fg = f["geometry"].to<JsonObject>();
        fg["coordinates"] = true;
    }

    ArduinoJson::Allocator* base = allocator ? allocator : MallocAllocator::instance();
    CappedAllocator doc_allocator(base, kMaxDocBytes);
    JsonDocument doc(&doc_allocator);

    uint8_t* feature_buf = nullptr;  // allocated with the first feature (PSRAM on the device)
    size_t processed = 0;
    size_t count = 0;
    size_t over_budget = 0;
    UsgsParseStatus status = UsgsParseStatus::Ok;

    for (;;) {
        // `c` is the first non-space byte after the previous feature's separator.
        if (c < 0) {
            status = eof_status(reader, reader_impl, UsgsParseStatus::Truncated);
            break;
        }
        if (c == ']') break;  // done (empty array included)
        if (c != '{') {       // neither a new feature nor the end of the array
            status = UsgsParseStatus::Truncated;
            break;
        }

        if (processed >= limits.max_features) {
            status = UsgsParseStatus::LimitReached;  // the caller keeps the previous snapshot
            break;
        }
        if (limits.yield_cb && processed > 0 && (processed % 16) == 0) limits.yield_cb();
        ++processed;

        if (feature_buf == nullptr) {
            feature_buf = static_cast<uint8_t*>(base->allocate(kMaxFeatureBytes));
            if (feature_buf == nullptr) {
                status = UsgsParseStatus::TooLarge;
                break;
            }
        }

        const ObjectRead obj = read_object(reader, feature_buf, kMaxFeatureBytes);
        if (!obj.complete) {
            status = eof_status(reader, reader_impl, UsgsParseStatus::Truncated);
            break;
        }

        if (obj.over_budget) {
            if (++over_budget >= kMaxOverflowSkips) {
                status = UsgsParseStatus::TooLarge;
                break;
            }
        } else {
            doc_allocator.clear_overflow();
            const DeserializationError err =
                deserializeJson(doc, reinterpret_cast<const char*>(feature_buf), obj.length,
                                DeserializationOption::Filter(filter),
                                DeserializationOption::NestingLimit(kNestingLimit));
            if (err == DeserializationError::NoMemory && doc_allocator.overflowed()) {
                if (++over_budget >= kMaxOverflowSkips) {  // repeated overflows: give up
                    status = UsgsParseStatus::TooLarge;
                    break;
                }
            } else if (err) {
                status = (err == DeserializationError::IncompleteInput) ? UsgsParseStatus::Truncated
                                                                        : UsgsParseStatus::BadJson;
                break;
            } else {
                Earthquake eq;
                if (read_feature(doc.as<JsonObject>(), eq, limits.now_ms)) store_record(out, cap, count, eq);
                over_budget = 0;  // only 8 IN A ROW give up
            }
        }

        c = skip_ws(reader);
        if (reader.too_large()) {
            status = UsgsParseStatus::TooLarge;
            break;
        }
        if (c < 0) {
            status = eof_status(reader, reader_impl, UsgsParseStatus::Truncated);
            break;
        }
        if (c == ',') {
            c = skip_ws(reader);
        }
    }

    if (feature_buf != nullptr) base->deallocate(feature_buf);

    if (status == UsgsParseStatus::Ok) sort_newest_first(out, count);

    result.status = status;
    result.count = count;
    return result;
}
