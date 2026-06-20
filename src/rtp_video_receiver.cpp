#include "rtp_video_receiver.h"
#include "latency_probe.hpp"
#include "spdlog/spdlog.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <random>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <stdexcept>
#if defined(__linux__)
#include <sys/random.h>
#endif

#define MAX_PACKET_SIZE 4096
#define RTP_HEADER_LEN 12

namespace {
    // -------------------------------------------------------------------------
    // Constants (verbatim from gstrtpreceiver.cpp)
    // -------------------------------------------------------------------------
    static constexpr int kIdrUdpPort = 11223;
    static constexpr int kIdrBurstCount = 3;
    static constexpr int kIdrBurstSpacingMs = 20;
    static constexpr int kIdrRepeatCount = 3;
    static constexpr int kIdrRepeatSpacingMs = 100;
    static constexpr int kIdrRecordRepeatCount = 3;
    static constexpr int kIdrRecordRepeatSpacingMs = 150;
    static constexpr uint64_t kStreamDownMs = 1200;
    static constexpr uint64_t kStreamTickMs = 200;
    static constexpr uint64_t kIntegrityCooldownMs = 350;
    static constexpr uint64_t kRtpGapCooldownMs = 500;
    static constexpr uint64_t kDecodeStallMs = 700;
    static constexpr uint64_t kDecodeStallCooldownMs = 700;
    static constexpr uint64_t kDecodeStallPktWindowMs = 500;
    static constexpr uint64_t kRtpSeqResetMs = 1000;

    // -------------------------------------------------------------------------
    // Globals (verbatim from gstrtpreceiver.cpp, except restream valve replaced)
    // -------------------------------------------------------------------------
    static std::mutex g_idr_sock_mutex;
    static int g_idr_sock = -1;
    static std::atomic<bool> g_idr_sock_ready{false};

    static std::mutex g_restream_mutex;
    static std::atomic<bool> g_restream_enabled{false};
    static std::string g_restream_target_ip;
    static std::string g_restream_manual_ip; // user's active selection; empty = auto-discover
    static std::string g_restream_pinned_ip;  // always shown in dropdown, set from config

    // Restream socket state — replaces gst valve/udpsink
    static std::atomic<bool> g_restream_open{false};        // valve "drop=false" equivalent
    static int g_restream_fd = -1;                          // set by the receiver
    static sockaddr_in g_restream_dst{};                    // current target

    static std::mutex g_last_hop_mutex;
    static std::string g_last_hop_ip;
    static std::atomic<uint64_t> g_last_pkt_ms{0};
    static std::atomic<bool> g_stream_up{false};
    static std::atomic<bool> g_pending_rec_idr{false};
    static std::atomic<uint64_t> g_last_integrity_idr_ms{0};
    static std::atomic<uint64_t> g_last_rtp_gap_idr_ms{0};
    static std::atomic<uint64_t> g_last_decode_stall_idr_ms{0};
    static std::atomic<uint64_t> g_last_decoded_ms{0};
    static std::atomic<uint64_t> g_last_rtp_seq_ms{0};
    static std::atomic<uint16_t> g_last_rtp_seq{0};
    static std::atomic<bool> g_last_rtp_seq_valid{false};
    static std::atomic<bool> g_idr_enabled{true};
    static std::atomic<bool> g_stream_idr_pending{false};
    static std::atomic<bool> g_record_idr_pending{false};

    // -------------------------------------------------------------------------
    // now_ms (verbatim)
    // -------------------------------------------------------------------------
    static uint64_t now_ms() {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    }

    // Forward declarations
    static void request_idr_bursts(const char* reason, int request_count, bool allow_pending);
    static void maybe_update_restream_target(bool force);

    // -------------------------------------------------------------------------
    // Pure helpers (verbatim from gstrtpreceiver.cpp)
    // -------------------------------------------------------------------------
    static bool contains_ip(const std::vector<std::string>& ips, const std::string& ip) {
        return !ip.empty() && std::find(ips.begin(), ips.end(), ip) != ips.end();
    }

    static bool is_stream_idr_reason(const char* reason) {
        return reason && !strcmp(reason, "stream-up");
    }

    static bool is_record_idr_reason(const char* reason) {
        return reason && !strncmp(reason, "record-start", strlen("record-start"));
    }

    static bool ensure_idr_socket() {
        if (g_idr_sock_ready.load(std::memory_order_acquire)) {
            return true;
        }

        std::lock_guard<std::mutex> lock(g_idr_sock_mutex);
        if (g_idr_sock_ready.load(std::memory_order_relaxed)) {
            return true;
        }

        g_idr_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (g_idr_sock < 0) {
            spdlog::warn("[IDR] socket(AF_INET,SOCK_DGRAM) failed: {}", strerror(errno));
            return false;
        }

        g_idr_sock_ready.store(true, std::memory_order_release);
        spdlog::info("[IDR] UDP socket ready");
        return true;
    }

    // -------------------------------------------------------------------------
    // Restream control — replaces gst valve/udpsink (per brief Step 3)
    // -------------------------------------------------------------------------
    static void restream_set_fd(int fd) { g_restream_fd = fd; }

    static void set_restream_open_locked(bool open) { g_restream_open.store(open, std::memory_order_relaxed); }

    static void update_restream_valve(bool enabled) {
        std::lock_guard<std::mutex> lock(g_restream_mutex);
        if (!enabled) set_restream_open_locked(false);
    }

    static std::vector<std::string> scan_hotspot_clients() {
        std::ifstream arp_file("/proc/net/arp");
        if (!arp_file.is_open()) return {};
        std::string line;
        std::getline(arp_file, line); // skip header
        std::vector<std::string> result;
        while (std::getline(arp_file, line)) {
            std::istringstream iss(line);
            std::string ip, hw_type, flags, hw_address, mask, device;
            if (!(iss >> ip >> hw_type >> flags >> hw_address >> mask >> device)) continue;
            if (device != "wlan0" && device != "usb0") continue;
            if (flags == "0x0" || hw_address == "00:00:00:00:00:00") continue;
            result.push_back(ip);
        }
        return result;
    }

    static std::string find_first_hotspot_client_ip() {
        const auto clients = scan_hotspot_clients();
        if (contains_ip(clients, g_restream_target_ip)) {
            return g_restream_target_ip;
        }
        return clients.empty() ? "" : clients.front();
    }

    static void maybe_update_restream_target(bool force) {
        static uint64_t last_probe_ms = 0;
        const uint64_t now = now_ms();
        if (!force && (now - last_probe_ms) < 1000) return;
        last_probe_ms = now;

        std::lock_guard<std::mutex> lock(g_restream_mutex);
        if (!g_restream_enabled.load(std::memory_order_relaxed)) { set_restream_open_locked(false); return; }
        const std::string next_ip = !g_restream_manual_ip.empty()
            ? g_restream_manual_ip : find_first_hotspot_client_ip();
        if (next_ip.empty()) {
            if (!g_restream_target_ip.empty()) {
                spdlog::info("[RESTREAM] No target client; stopping unicast restream");
                g_restream_target_ip.clear();
            }
            set_restream_open_locked(false);
            return;
        }
        if (next_ip != g_restream_target_ip) {
            g_restream_target_ip = next_ip;
            std::memset(&g_restream_dst, 0, sizeof(g_restream_dst));
            g_restream_dst.sin_family = AF_INET;
            g_restream_dst.sin_port = htons(5600);
            inet_pton(AF_INET, g_restream_target_ip.c_str(), &g_restream_dst.sin_addr);
            spdlog::info("[RESTREAM] Streaming to {}:{}", g_restream_target_ip, 5600);
        }
        set_restream_open_locked(true);
    }

    // Called from the recv loop for every datagram.
    static void restream_forward(const uint8_t* data, size_t len) {
        if (!g_restream_open.load(std::memory_order_relaxed)) return;
        std::lock_guard<std::mutex> lock(g_restream_mutex);
        if (g_restream_fd < 0 || g_restream_dst.sin_family != AF_INET) return;
        sendto(g_restream_fd, data, len, 0,
               reinterpret_cast<const sockaddr*>(&g_restream_dst), sizeof(g_restream_dst));
    }

    // -------------------------------------------------------------------------
    // IDR token / burst helpers (verbatim from gstrtpreceiver.cpp)
    // -------------------------------------------------------------------------
    static uint32_t secure_random_u32() {
        uint32_t out = 0;
#if defined(__linux__)
        ssize_t n = getrandom(&out, sizeof(out), 0);
        if (n == sizeof(out)) {
            return out;
        }
#endif
        static std::random_device rd;
        out = (static_cast<uint32_t>(rd()) << 16) ^ static_cast<uint32_t>(rd());
        return out;
    }

    static void make_idr_token3(char out[4]) {
        static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz";
        const uint32_t r0 = secure_random_u32();
        const uint32_t r1 = secure_random_u32();
        const uint32_t r2 = secure_random_u32();
        out[0] = alphabet[r0 % 26];
        out[1] = alphabet[r1 % 26];
        out[2] = alphabet[r2 % 26];
        out[3] = '\0';
    }

    static void send_idr_token_to_ip(const char* ip, const char token3[4]) {
        if (!ip || !ip[0]) {
            return;
        }

        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(static_cast<uint16_t>(kIdrUdpPort));

        if (inet_pton(AF_INET, ip, &dst.sin_addr) != 1) {
            spdlog::warn("[IDR] inet_pton failed for ip={}", ip);
            return;
        }

        char payload[16];
        snprintf(payload, sizeof(payload), "%s\n", token3);
        int rc = sendto(g_idr_sock, payload, static_cast<int>(strlen(payload)), 0,
                        reinterpret_cast<sockaddr*>(&dst), static_cast<int>(sizeof(dst)));
        if (rc < 0) {
            spdlog::warn("[IDR] sendto({}:{}) failed: {}", ip, kIdrUdpPort, strerror(errno));
        }
    }

    static void send_idr_burst(const std::string& ip) {
        for (int i = 0; i < kIdrBurstCount; ++i) {
            char tok[4];
            make_idr_token3(tok);
            send_idr_token_to_ip(ip.c_str(), tok);
            if (i + 1 < kIdrBurstCount) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kIdrBurstSpacingMs));
            }
        }
    }

    static std::string get_last_hop_ip_copy() {
        std::lock_guard<std::mutex> lock(g_last_hop_mutex);
        return g_last_hop_ip;
    }

    static void request_idr_bursts(const char* reason, int request_count, bool allow_pending) {
        if (!g_idr_enabled.load(std::memory_order_relaxed)) {
            return;
        }

        const bool track_stream = is_stream_idr_reason(reason);
        const bool track_record = is_record_idr_reason(reason);
        if (track_stream) {
            g_stream_idr_pending.store(true, std::memory_order_relaxed);
        }
        if (track_record) {
            g_record_idr_pending.store(true, std::memory_order_relaxed);
        }

        const std::string ip = get_last_hop_ip_copy();
        if (ip.empty()) {
            spdlog::warn("[IDR] Cannot request IDR (last-hop unknown) reason={}", reason ? reason : "(null)");
            if (allow_pending) {
                g_pending_rec_idr.store(true, std::memory_order_relaxed);
            }
            return;
        }

        if (!ensure_idr_socket()) {
            return;
        }

        g_pending_rec_idr.store(false, std::memory_order_relaxed);
        const std::string reason_str = reason ? reason : "";

        if (track_record) {
            std::thread([ip, reason_str, request_count]() {
                const char* reason_c = reason_str.empty() ? "no-reason" : reason_str.c_str();
                for (int r = 0; r < request_count; ++r) {
                    if (!g_record_idr_pending.load(std::memory_order_relaxed)) {
                        spdlog::info("[IDR] Record refresh confirmed; skipping remaining bursts");
                        break;
                    }
                    spdlog::info("[IDR] Request 1 burst(s) to {}:{} ({} {}/{})",
                                 ip, kIdrUdpPort, reason_c, r + 1, request_count);
                    send_idr_burst(ip);
                    if (r + 1 < request_count) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(kIdrRecordRepeatSpacingMs));
                    }
                }
            }).detach();
            return;
        }

        std::thread([ip, reason_str, request_count]() {
            const char* reason_c = reason_str.empty() ? "no-reason" : reason_str.c_str();
            const bool track_stream = is_stream_idr_reason(reason_c);
            spdlog::info("[IDR] Request {} burst(s) to {}:{} ({})", request_count, ip, kIdrUdpPort, reason_c);
            for (int r = 0; r < request_count; ++r) {
                if (track_stream && !g_stream_idr_pending.load(std::memory_order_relaxed)) {
                    spdlog::info("[IDR] Stream refresh confirmed; skipping remaining bursts");
                    break;
                }
                send_idr_burst(ip);
                if (r + 1 < request_count) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(kIdrRepeatSpacingMs));
                }
            }
        }).detach();
    }

    // -------------------------------------------------------------------------
    // NAL / IDR frame detection (verbatim from gstrtpreceiver.cpp)
    // -------------------------------------------------------------------------
    static void for_each_nal(const uint8_t* data, size_t size,
                             const std::function<void(const uint8_t*, size_t)>& cb) {
        auto find_start = [&](size_t from, size_t& start_len) -> size_t {
            for (size_t i = from; i + 3 < size; i++) {
                if (data[i] == 0x00 && data[i + 1] == 0x00) {
                    if (data[i + 2] == 0x01) {
                        start_len = 3;
                        return i;
                    }
                    if (i + 3 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
                        start_len = 4;
                        return i;
                    }
                }
            }
            start_len = 0;
            return size;
        };

        size_t pos = 0;
        while (pos < size) {
            size_t start_len = 0;
            size_t start = find_start(pos, start_len);
            if (start == size) {
                break;
            }
            size_t nal_start = start + start_len;
            size_t next_len = 0;
            size_t next = find_start(nal_start, next_len);
            size_t nal_end = (next == size) ? size : next;
            if (nal_end > nal_start) {
                cb(data + nal_start, nal_end - nal_start);
            }
            pos = nal_end;
        }
    }

    static bool has_idr_frame(const uint8_t* data, size_t size, VideoCodec codec) {
        bool found = false;
        if (!data || size == 0) {
            return false;
        }
        for_each_nal(data, size, [&](const uint8_t* nal, size_t nal_size) {
            if (found || !nal || nal_size == 0) {
                return;
            }
            if (codec == VideoCodec::H265) {
                uint8_t nal_type = (nal[0] >> 1) & 0x3f;
                if (nal_type >= 16 && nal_type <= 21) {
                    found = true;
                }
            } else if (codec == VideoCodec::H264) {
                uint8_t nal_type = nal[0] & 0x1f;
                if (nal_type == 5) {
                    found = true;
                }
            }
        });
        return found;
    }

    static void maybe_mark_idr_received(const uint8_t* data, size_t size, VideoCodec codec) {
        if (!g_idr_enabled.load(std::memory_order_relaxed)) {
            return;
        }

        if (!g_stream_idr_pending.load(std::memory_order_relaxed) &&
            !g_record_idr_pending.load(std::memory_order_relaxed)) {
            return;
        }

        if (!has_idr_frame(data, size, codec)) {
            return;
        }

        if (g_stream_idr_pending.exchange(false, std::memory_order_relaxed)) {
            spdlog::info("[IDR] Stream refresh confirmed (IDR received)");
        }
        if (g_record_idr_pending.exchange(false, std::memory_order_relaxed)) {
            g_pending_rec_idr.store(false, std::memory_order_relaxed);
            spdlog::info("[IDR] Record refresh confirmed (IDR received)");
        }
    }

    // -------------------------------------------------------------------------
    // Stream presence / decode-stall tracking (verbatim from gstrtpreceiver.cpp)
    // -------------------------------------------------------------------------
    static void maybe_request_decode_stall(uint64_t now) {
        if (!g_idr_enabled.load(std::memory_order_relaxed)) {
            return;
        }

        if (!g_stream_up.load(std::memory_order_relaxed)) {
            return;
        }

        const uint64_t last_pkt = g_last_pkt_ms.load(std::memory_order_relaxed);
        const uint64_t last_decoded = g_last_decoded_ms.load(std::memory_order_relaxed);
        if (last_decoded == 0) {
            return;
        }

        if (last_pkt && (now - last_pkt) > kDecodeStallPktWindowMs) {
            return;
        }

        if (last_pkt > last_decoded && (now - last_decoded) > kDecodeStallMs) {
            const uint64_t last_idr = g_last_decode_stall_idr_ms.load(std::memory_order_relaxed);
            if (!last_idr || (now - last_idr) > kDecodeStallCooldownMs) {
                g_last_decode_stall_idr_ms.store(now, std::memory_order_relaxed);
                spdlog::info("[IDR] Decode stall (no frames for {} ms) -> request IDR", now - last_decoded);
                request_idr_bursts("decode-stall", 1, false);
            }
        }
    }

    static void tick_stream_presence() {
        if (!g_idr_enabled.load(std::memory_order_relaxed)) {
            return;
        }

        static uint64_t last_tick = 0;
        const uint64_t now = now_ms();
        if (now - last_tick < kStreamTickMs) {
            return;
        }
        last_tick = now;

        if (!g_stream_up.load(std::memory_order_relaxed)) {
            return;
        }

        const uint64_t last = g_last_pkt_ms.load(std::memory_order_relaxed);
        if (last && now > last && (now - last) > kStreamDownMs) {
            if (g_stream_up.exchange(false)) {
                spdlog::info("[NET] Stream DOWN (no packets for {} ms)", now - last);
                g_last_rtp_seq_valid.store(false, std::memory_order_relaxed);
                g_last_rtp_seq_ms.store(0, std::memory_order_relaxed);
            }
        }

        maybe_request_decode_stall(now);
    }

    static void reset_stream_tracking() {
        g_stream_up.store(false, std::memory_order_relaxed);
        g_last_pkt_ms.store(0, std::memory_order_relaxed);
        g_last_decoded_ms.store(0, std::memory_order_relaxed);
        g_last_rtp_seq_valid.store(false, std::memory_order_relaxed);
        g_last_rtp_seq_ms.store(0, std::memory_order_relaxed);
        g_stream_idr_pending.store(false, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(g_last_hop_mutex);
        g_last_hop_ip.clear();
    }

    static void maybe_request_idr_rate_limited(const char* reason, const char* context) {
        if (!g_idr_enabled.load(std::memory_order_relaxed)) {
            return;
        }

        if (!g_stream_up.load(std::memory_order_relaxed)) {
            return;
        }

        const uint64_t now = now_ms();
        const uint64_t last = g_last_integrity_idr_ms.load(std::memory_order_relaxed);
        if (last && (now - last) < kIntegrityCooldownMs) {
            return;
        }

        g_last_integrity_idr_ms.store(now, std::memory_order_relaxed);
        if (context && context[0]) {
            spdlog::info("[IDR] {} -> request IDR", context);
        } else {
            spdlog::info("[IDR] Decoder issue -> request IDR");
        }

        request_idr_bursts(reason ? reason : "decoder-issue", 1, false);
    }

    // -------------------------------------------------------------------------
    // Per-packet hook (replaces gst buffer version — per brief Step 4)
    // -------------------------------------------------------------------------
    static void update_last_hop_ip(const sockaddr_in& from) {
        if (!g_idr_enabled.load(std::memory_order_relaxed)) return;
        char ip[INET_ADDRSTRLEN] = {0};
        if (!inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip))) return;
        std::lock_guard<std::mutex> lock(g_last_hop_mutex);
        if (g_last_hop_ip != ip) { g_last_hop_ip = ip; spdlog::info("[NET] Last-hop sender: {}", g_last_hop_ip); }
    }

    static void maybe_request_idr_for_rtp_gap(uint16_t gap_count) {
        if (!g_idr_enabled.load(std::memory_order_relaxed)) {
            return;
        }

        if (!g_stream_up.load(std::memory_order_relaxed)) {
            return;
        }

        const uint64_t now = now_ms();
        const uint64_t last = g_last_rtp_gap_idr_ms.load(std::memory_order_relaxed);
        if (last && (now - last) < kRtpGapCooldownMs) {
            return;
        }

        g_last_rtp_gap_idr_ms.store(now, std::memory_order_relaxed);
        spdlog::info("[IDR] RTP gap detected (missing {} packet(s)) -> request IDR", gap_count);
        request_idr_bursts("rtp-gap", 1, false);
    }

    static void on_incoming_stream_bytes(const sockaddr_in& from, const char* tag) {
        if (!g_idr_enabled.load(std::memory_order_relaxed)) return;
        g_last_pkt_ms.store(now_ms(), std::memory_order_relaxed);
        update_last_hop_ip(from);
        if (!g_stream_up.exchange(true)) {
            spdlog::info("[NET] Stream UP ({})", tag ? tag : "unknown");
            request_idr_bursts("stream-up", kIdrRepeatCount, false);
        }
        if (g_pending_rec_idr.load(std::memory_order_relaxed)) {
            if (!g_record_idr_pending.load(std::memory_order_relaxed)) {
                g_pending_rec_idr.store(false, std::memory_order_relaxed);
            } else if (!get_last_hop_ip_copy().empty()) {
                g_pending_rec_idr.store(false, std::memory_order_relaxed);
                request_idr_bursts("record-start(pending)", kIdrRecordRepeatCount, false);
            }
        }
    }

    // RTP seq is bytes 2-3 of the datagram. Returns true if a gap was detected.
    static bool track_rtp_sequence(const uint8_t* rtp, size_t len, uint16_t& last_seq, bool& have_seq) {
        if (!g_idr_enabled.load(std::memory_order_relaxed) || len < 4) return false;
        const uint16_t seq = uint16_t((rtp[2] << 8) | rtp[3]);
        bool gap = false;
        if (have_seq) {
            const uint16_t diff = uint16_t(seq - last_seq);
            if (diff != 0 && diff < 30000 && diff > 1) {
                gap = true;
                maybe_request_idr_for_rtp_gap(uint16_t(diff - 1));
            }
        }
        last_seq = seq; have_seq = true;
        return gap;
    }

} // namespace

// =============================================================================
// C-API (verbatim from gstrtpreceiver.cpp, using same g_* globals above)
// =============================================================================

void idr_set_enabled(bool enabled) {
    g_idr_enabled.store(enabled, std::memory_order_relaxed);
}

bool idr_get_enabled() {
    return g_idr_enabled.load(std::memory_order_relaxed);
}

void restream_set_enabled(bool enabled) {
    g_restream_enabled.store(enabled, std::memory_order_relaxed);
    update_restream_valve(enabled);
}

bool restream_get_enabled() {
    return g_restream_enabled.load(std::memory_order_relaxed);
}

void restream_scan_clients(char* buf, size_t buf_len) {
    if (!buf || buf_len == 0) {
        return;
    }

    const auto ips = scan_hotspot_clients();
    std::string manual_ip;
    std::string pinned_ip;
    {
        std::lock_guard<std::mutex> lock(g_restream_mutex);
        manual_ip = g_restream_manual_ip;
        pinned_ip = g_restream_pinned_ip;
    }

    std::string combined = "Auto";
    for (const auto& ip : ips) {
        combined += '\n';
        combined += ip;
    }
    // Always include the pinned IP from config so it stays in the list
    // regardless of whether the user currently has it selected.
    if (!pinned_ip.empty() && !contains_ip(ips, pinned_ip)) {
        combined += '\n';
        combined += pinned_ip;
    }
    // Also include the active manual IP if it differs from the pinned/default ones.
    if (!manual_ip.empty() && manual_ip != "Auto" && manual_ip != pinned_ip
            && !contains_ip(ips, manual_ip)) {
        combined += '\n';
        combined += manual_ip;
    }
    strncpy(buf, combined.c_str(), buf_len - 1);
    buf[buf_len - 1] = '\0';
}

void restream_set_manual_ip(const char* ip) {
    std::lock_guard<std::mutex> lock(g_restream_mutex);
    g_restream_manual_ip = (ip && ip[0] != '\0' && strcmp(ip, "Auto") != 0) ? ip : "";
    g_restream_target_ip.clear(); // force retarget on next probe
}

void restream_set_pinned_ip(const char* ip) {
    std::lock_guard<std::mutex> lock(g_restream_mutex);
    g_restream_pinned_ip = (ip && ip[0] != '\0') ? ip : "";
}

const char* restream_get_manual_ip() {
    std::lock_guard<std::mutex> lock(g_restream_mutex);
    static char buf[64];
    strncpy(buf, g_restream_manual_ip.c_str(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

void idr_request_record_start() {
    request_idr_bursts("record-start", kIdrRecordRepeatCount, true);
}

void idr_request_decoder_issue(const char* reason) {
    const char* ctx = reason ? reason : "decoder-issue";
    maybe_request_idr_rate_limited(reason, ctx);
}

void idr_notify_decoded_frame() {
    if (!g_idr_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    g_last_decoded_ms.store(now_ms(), std::memory_order_relaxed);
}

// =============================================================================
// RtpVideoReceiver class implementation (per brief Step 5)
// =============================================================================

RtpVideoReceiver::RtpVideoReceiver(int udp_port)
    : m_port(udp_port),
      m_depay([this](const uint8_t* au, size_t len) {
          auto buf = std::make_shared<std::vector<uint8_t>>(au, au + len);
          if (au && len) maybe_mark_idr_received(au, len, VideoCodec::H265);
          if (m_cb) m_cb(buf);
      }) {}

RtpVideoReceiver::RtpVideoReceiver(const char* unix_sock)
    : m_unix_socket(unix_sock ? unix_sock : ""),
      m_depay([this](const uint8_t* au, size_t len) {
          auto buf = std::make_shared<std::vector<uint8_t>>(au, au + len);
          if (au && len) maybe_mark_idr_received(au, len, VideoCodec::H265);
          if (m_cb) m_cb(buf);
      }) {}

RtpVideoReceiver::~RtpVideoReceiver() { stop(); }

void RtpVideoReceiver::open_socket() {
    if (m_unix_socket.empty()) {
        m_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (m_sock < 0) throw std::runtime_error(std::string("socket() failed: ") + strerror(errno));
        int reuse = 1;
        setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        int rcvbuf = 4 * 1024 * 1024;  // absorb bursts during transient MPP stalls
        setsockopt(m_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(uint16_t(m_port));
        if (bind(m_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            throw std::runtime_error(std::string("bind() failed: ") + strerror(errno));
    } else {
        m_sock = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (m_sock < 0) throw std::runtime_error(std::string("socket() failed: ") + strerror(errno));
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        addr.sun_path[0] = '\0';  // abstract namespace
        strncpy(addr.sun_path + 1, m_unix_socket.c_str(), sizeof(addr.sun_path) - 2);
        socklen_t addr_len = sizeof(addr.sun_family) + 1 + m_unix_socket.size();
        if (bind(m_sock, reinterpret_cast<sockaddr*>(&addr), addr_len) < 0)
            throw std::runtime_error(std::string("bind() failed: ") + strerror(errno));
    }
    // Recv timeout so the loop can observe stop().
    timeval tv{0, 200 * 1000};
    setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    m_restream_sock = socket(AF_INET, SOCK_DGRAM, 0);
    restream_set_fd(m_restream_sock);
}

void RtpVideoReceiver::start(NEW_FRAME_CALLBACK on_frame) {
    m_cb = std::move(on_frame);
    open_socket();
    m_run.store(true);
    m_thread = std::thread([this]() {
        pthread_setname_np(pthread_self(), "rtp-recv");
        recv_loop();
    });
}

void RtpVideoReceiver::stop() {
    m_run.store(false);
    if (m_thread.joinable()) m_thread.join();
    if (m_sock >= 0) { close(m_sock); m_sock = -1; }
    if (m_restream_sock >= 0) { restream_set_fd(-1); close(m_restream_sock); m_restream_sock = -1; }
    reset_stream_tracking();
    m_depay.reset();
    m_have_seq = false;
}

void RtpVideoReceiver::recv_loop() {
    std::vector<uint8_t> buf(MAX_PACKET_SIZE);
    while (m_run.load()) {
        sockaddr_in from{};
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(m_sock, buf.data(), buf.size(), 0,
                             reinterpret_cast<sockaddr*>(&from), &fromlen);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                tick_stream_presence();
                maybe_update_restream_target(false);
                continue;
            }
            spdlog::warn("[RTP] recvfrom error: {}", strerror(errno));
            continue;
        }
        if (n <= RTP_HEADER_LEN) { continue; }
        const uint8_t* rtp = buf.data();
        const size_t len = size_t(n);

        if (latency_probe::active.load(std::memory_order_acquire))
            latency_probe::on_rtp_buffer(rtp, len, latency_probe::now_us());

        on_incoming_stream_bytes(from, "udpsrc");
        const bool gap = track_rtp_sequence(rtp, len, m_last_seq, m_have_seq);
        if (gap) m_depay.on_discontinuity();

        restream_forward(rtp, len);

        // RTP header (no CSRC/extension assumed): marker = bit7 of byte 1, ts = bytes 4-7.
        const bool marker = (rtp[1] & 0x80) != 0;
        const uint32_t ts = (uint32_t(rtp[4]) << 24) | (uint32_t(rtp[5]) << 16) |
                            (uint32_t(rtp[6]) << 8) | uint32_t(rtp[7]);
        m_depay.on_payload(rtp + RTP_HEADER_LEN, len - RTP_HEADER_LEN, marker, ts);

        tick_stream_presence();
        maybe_update_restream_target(false);
    }
}
