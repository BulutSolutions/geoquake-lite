#pragma once
// Direct USGS GeoJSON reader (summary feeds). Pure parsing code: no Arduino
// headers, so it builds both in the firmware and in the native unit tests.
//
// The feed is a FeatureCollection:
//   {"type":"FeatureCollection","metadata":{...},
//    "features":[{"type":"Feature","properties":{...},"geometry":{"coordinates":[lon,lat,depth]}},...],
//    "bbox":[...]}
// Only one feature object is ever held in memory at a time.

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>
#include "quake_types.h"

enum class UsgsParseStatus : uint8_t {
    Ok,           // the features array was read to its ']'
    NoFeatures,   // no root-level "features" array in the document
    Truncated,    // the stream ended (or a feature object was incomplete) mid-array
    TooLarge,     // the byte budget ran out, or features exceeded the per-feature memory budget
    BadJson,      // a feature object is not valid JSON
    LimitReached, // max_features was reached before the closing ']' (caller keeps the old snapshot)
    Timeout,      // the reader hit its total deadline (the fetch ran out of time)
};

// ArduinoJson custom-reader concept: anything with read() / readBytes() works.
struct UsgsReader {
    virtual ~UsgsReader() {}
    virtual int read() = 0;
    virtual size_t readBytes(char* buffer, size_t length) = 0;
    // True once the reader gave up on its total deadline (as opposed to a real EOF).
    virtual bool timed_out() const { return false; }
    // True when the source is definitively closed: no more bytes will arrive.
    virtual bool source_closed() const { return false; }
};

// Wraps a NON-BLOCKING byte source (WiFiClient::read() returns -1 on a packet
// gap). -1 means "no byte yet", not EOF: polling continues until a byte
// arrives, the total deadline passes, or idle_timeout_ms elapse without data.
// The total deadline is checked on EVERY call, even while data keeps arriving.
template <typename TSource>
class UsgsBlockingReader : public UsgsReader {
public:
    UsgsBlockingReader(TSource& source, uint32_t deadline_ms, uint32_t (*now_ms)(), void (*wait_fn)(),
                       uint32_t idle_timeout_ms = 10000)
        : source_(source),
          deadline_ms_(deadline_ms),
          now_ms_(now_ms),
          wait_fn_(wait_fn),
          idle_timeout_ms_(idle_timeout_ms),
          last_data_ms_(now_ms()) {}

    int read() override {
        for (;;) {
            if (deadline_expired()) return -1;
            const int c = source_.read();
            if (c >= 0) {
                last_data_ms_ = now_ms_();
                return c;
            }
            if (source_closed()) return -1;  // the stream is over, not a packet gap
            if (idle_expired()) return -1;
            wait_fn_();
        }
    }

    size_t readBytes(char* buffer, size_t length) override {
        size_t n = 0;
        while (n < length) {
            if (deadline_expired()) break;
            const size_t got = source_.readBytes(buffer + n, length - n);
            if (got > 0) {
                n += got;
                last_data_ms_ = now_ms_();
                continue;
            }
            if (source_closed()) break;
            if (idle_expired()) break;
            wait_fn_();
        }
        return n;
    }

    bool timed_out() const override { return timed_out_; }

private:
    bool deadline_expired() {
        if (static_cast<int32_t>(now_ms_() - deadline_ms_) >= 0) {
            timed_out_ = true;
            return true;
        }
        return false;
    }

    bool idle_expired() {
        if (static_cast<int32_t>(now_ms_() - last_data_ms_) >= static_cast<int32_t>(idle_timeout_ms_)) {
            timed_out_ = true;
            return true;
        }
        return false;
    }

    TSource& source_;
    const uint32_t deadline_ms_;
    uint32_t (*const now_ms_)();
    void (*const wait_fn_)();
    const uint32_t idle_timeout_ms_;
    uint32_t last_data_ms_;
    bool timed_out_ = false;
};

struct UsgsParseLimits {
    size_t max_features = 20000;              // stop after this many feature objects (LimitReached)
    size_t max_bytes = 4u * 1024u * 1024u;    // byte budget counted as we read
    void (*yield_cb)() = nullptr;             // called every 16 features (feed the watchdog)
    long long now_ms = 0;                     // current epoch ms; > 0 rejects impossible future times
};

struct UsgsParseResult {
    UsgsParseStatus status = UsgsParseStatus::BadJson;
    size_t count = 0;  // records stored in out[]
};

// Parses the feature stream into out[0..cap); keeps at most cap records (newest
// by time win) and sorts them newest first. Invalid features are skipped.
UsgsParseResult usgs_parse_features(UsgsReader& reader, Earthquake* out, size_t cap,
                                    const UsgsParseLimits& limits = UsgsParseLimits(),
                                    ArduinoJson::Allocator* allocator = nullptr);
