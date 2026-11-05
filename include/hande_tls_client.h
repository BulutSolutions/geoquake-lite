#pragma once

// WiFiClientSecure for every TLS connection in the firmware. It fixes two Arduino-ESP32 2.0.17 bugs:
//
// 1. A second stop() closes file descriptor 0. stop_ssl_socket() ends by zeroing the whole TLS context
//    (ssl_client.cpp:346), so after a stop the socket number reads 0, not -1, and the next stop() (HTTPClient::end()
//    and then the destructor, a failed connect, or a kept client stopped again) calls close(0): it closes whatever file
//    holds descriptor 0 (an SD file was lost this way in another app on the board). Sockets never get descriptor 0
//    (lwIP numbers them from LWIP_SOCKET_OFFSET up), so 0 here always means "already stopped".
// 2. A DNS lookup races lwIP's own DNS timer. WiFi.hostByName() calls dns_gethostbyname() from the caller's task while
//    the tcpip thread's dns_tmr walks the same table (CONFIG_LWIP_TCPIP_CORE_LOCKING is off), and the board crashed in
//    dns_tmr -> dns_send -> udp_sendto (seen in a stack dump, 2026-10-24). connect(host) here starts the lookup on
//    the tcpip thread (tcpip_api_call) and connects to the address, keeping the host name for SNI and the certificate.
#include <WiFiClientSecure.h>

#if defined(HANDE_SIM)
using HandeTlsClient = WiFiClientSecure;  // the host build's stand-in has no TLS context
#else
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lwip/dns.h>
#include <lwip/priv/tcpip_priv.h>

#include <atomic>

namespace hande_tls_detail {

struct DnsState {
    SemaphoreHandle_t lock = nullptr;  // one lookup at a time (like Arduino's WIFI_DNS_IDLE_BIT)
    SemaphoreHandle_t done = nullptr;  // given by the answer callback
    std::atomic<uint32_t> generation{0};
    ip_addr_t found;
    bool found_ok = false;
    uint32_t found_generation = 0;  // the lookup this answer belongs to
};

inline DnsState& dns_state() {
    static DnsState state;
    static portMUX_TYPE init_mux = portMUX_INITIALIZER_UNLOCKED;
    if (!state.lock) {
        SemaphoreHandle_t lock = xSemaphoreCreateMutex();
        SemaphoreHandle_t done = xSemaphoreCreateBinary();
        portENTER_CRITICAL(&init_mux);
        if (!state.lock && lock && done) {
            state.done = done;
            state.lock = lock;
            lock = done = nullptr;
        }
        portEXIT_CRITICAL(&init_mux);
        if (lock) vSemaphoreDelete(lock);
        if (done) vSemaphoreDelete(done);
    }
    return state;
}

struct DnsCall {
    tcpip_api_call_data call;  // first: tcpip_api_call() hands this pointer back
    const char* host;
    uint32_t generation;
    ip_addr_t addr;
    err_t err;
};

// tcpip thread. A late answer to a lookup that already timed out carries an old generation and is ignored.
inline void dns_found(const char*, const ip_addr_t* addr, void* arg) {
    DnsState& state = dns_state();
    if (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg)) != state.generation.load()) return;
    state.found_ok = addr && IP_IS_V4(addr);
    if (state.found_ok) state.found = *addr;
    state.found_generation = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    xSemaphoreGive(state.done);
}

// tcpip thread: the lookup starts where lwIP's DNS timer runs, so the two never touch the table at the same time.
inline err_t dns_start(tcpip_api_call_data* call) {
    DnsCall* lookup = reinterpret_cast<DnsCall*>(call);
    lookup->err = dns_gethostbyname(lookup->host, &lookup->addr, dns_found,
                                    reinterpret_cast<void*>(static_cast<uintptr_t>(lookup->generation)));
    return ERR_OK;
}

// Resolves `host` (a name or a dotted IPv4 address) to an IPv4 address; false when it cannot within timeout_ms.
inline bool resolve(const char* host, IPAddress& out, uint32_t timeout_ms) {
    if (out.fromString(host)) return true;
    DnsState& state = dns_state();
    if (!state.lock) return false;
    xSemaphoreTake(state.lock, portMAX_DELAY);
    xSemaphoreTake(state.done, 0);  // an answer that came after the previous lookup gave up
    DnsCall lookup = {};
    lookup.host = host;
    lookup.generation = state.generation.fetch_add(1) + 1;
    tcpip_api_call(dns_start, &lookup.call);
    bool ok = false;
    if (lookup.err == ERR_OK) {
        ok = IP_IS_V4(&lookup.addr);
        if (ok) out = IPAddress(ip_2_ip4(&lookup.addr)->addr);
    } else if (lookup.err == ERR_INPROGRESS && xSemaphoreTake(state.done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE &&
               state.found_generation == lookup.generation && state.found_ok) {  // not a late answer to an older one
        out = IPAddress(ip_2_ip4(&state.found)->addr);
        ok = true;
    }
    state.generation.fetch_add(1);  // from here on, a late answer belongs to no lookup
    xSemaphoreGive(state.lock);
    return ok;
}

}  // namespace hande_tls_detail

class HandeTlsClient : public WiFiClientSecure {
public:
    ~HandeTlsClient() { forget_zeroed_socket(); }  // runs before ~WiFiClientSecure(), which calls stop() again
    void stop() override {
        forget_zeroed_socket();
        WiFiClientSecure::stop();
        forget_zeroed_socket();
    }

    // HTTPClient connects by name with a timeout; both name paths resolve on the tcpip thread (bug 2 above).
    int connect(const char* host, uint16_t port, int32_t timeout) override {
        _timeout = timeout;
        return connect(host, port);
    }
    int connect(const char* host, uint16_t port) override {
        IPAddress address;
        if (!hande_tls_detail::resolve(host, address, dns_timeout_ms_)) return 0;
        return WiFiClientSecure::connect(address, port, host, _CA_cert, _cert, _private_key);
    }
    using WiFiClientSecure::connect;
    // A caller with a hard deadline of its own (the time-zone lookup).
    void setDnsTimeout(uint32_t timeout_ms) { dns_timeout_ms_ = timeout_ms; }

private:
    static constexpr uint32_t kDnsTimeoutMs = 15000;  // lwIP gives up after ~14 s (WiFi.hostByName waits 15 s too)
    uint32_t dns_timeout_ms_ = kDnsTimeoutMs;

    void forget_zeroed_socket() {
        if (sslclient && sslclient->socket == 0) sslclient->socket = -1;
    }
};
#endif
