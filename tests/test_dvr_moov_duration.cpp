#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

#include "../src/minimp4.h"

/* The DVR writes fragmented MP4 (moof/mdat fragments) so a power loss mid-flight
 * still leaves a playable file. The trade-off is that the `moov` header is emitted
 * once, before the first frame, when the total duration is still unknown — so its
 * `mvhd`/`mdhd` duration fields stay 0. ffprobe/VLC reconstruct the length by
 * scanning every fragment, but simpler desktop players read only the `moov` header,
 * see duration 0, and show no total-time / no seek bar.
 *
 * On a clean stop the total IS known (the sum of every frame's duration — summed,
 * not count*fps, because the DVR feeds variable per-frame durations from RTP/PTS
 * deltas). The fix back-patches the fixed-size frag-mode `moov` in place at close.
 * These tests pin that the patched header reports the real duration. */

namespace {

// In-memory MP4 sink supporting random-access (seek-back) overwrites, exactly
// like the file-backed fseek+fwrite callback the DVR uses (dvr.cpp:49).
struct MemFile {
    std::vector<uint8_t> buf;
};

int mem_write(int64_t offset, const void *data, size_t size, void *token) {
    auto *m = static_cast<MemFile *>(token);
    if (offset + (int64_t)size > (int64_t)m->buf.size())
        m->buf.resize(offset + size);
    std::memcpy(m->buf.data() + offset, data, size);
    return 0;
}

// Minimal big-endian box walker: find `type` nested under an optional parent and
// return the offset of its first byte. Returns -1 if not found. Only descends into
// the container boxes we care about (moov/trak/mdia).
int64_t find_box(const std::vector<uint8_t> &b, uint32_t type,
                 int64_t off, int64_t end) {
    auto be32 = [&](int64_t p) {
        return ((uint32_t)b[p] << 24) | ((uint32_t)b[p + 1] << 16) |
               ((uint32_t)b[p + 2] << 8) | (uint32_t)b[p + 3];
    };
    while (off + 8 <= end) {
        uint32_t size = be32(off);
        uint32_t t = be32(off + 4);
        int64_t hdr = 8;
        if (size == 1) { hdr = 16; } // 64-bit size unused for our small boxes
        int64_t box_end = (size == 0) ? end : off + size;
        if (t == type)
            return off;
        if (t == FOUR_CHAR_INT('m', 'o', 'o', 'v') ||
            t == FOUR_CHAR_INT('t', 'r', 'a', 'k') ||
            t == FOUR_CHAR_INT('m', 'd', 'i', 'a')) {
            int64_t inner = find_box(b, type, off + hdr, box_end);
            if (inner >= 0)
                return inner;
        }
        off = box_end;
    }
    return -1;
}

// Read a full-box (version+flags) duration field. `dur_off_v0` is the byte offset
// of the 32-bit duration relative to the box payload for version 0.
struct DurField { uint32_t timescale; uint64_t duration; };

DurField read_mvhd_like(const std::vector<uint8_t> &b, int64_t box_off,
                        int ts_off_v0, int dur_off_v0) {
    int64_t payload = box_off + 8;          // skip size+type
    uint8_t version = b[payload];
    int64_t p = payload + 4;                // skip version+flags
    DurField r{};
    if (version == 1) {
        // creation(8)+modification(8)+timescale(4)+duration(8)
        int64_t ts = p + 16, du = p + 20;
        r.timescale = ((uint32_t)b[ts] << 24) | ((uint32_t)b[ts + 1] << 16) |
                      ((uint32_t)b[ts + 2] << 8) | b[ts + 3];
        for (int i = 0; i < 8; i++) r.duration = (r.duration << 8) | b[du + i];
    } else {
        int64_t ts = p + ts_off_v0, du = p + dur_off_v0;
        r.timescale = ((uint32_t)b[ts] << 24) | ((uint32_t)b[ts + 1] << 16) |
                      ((uint32_t)b[ts + 2] << 8) | b[ts + 3];
        r.duration = ((uint32_t)b[du] << 24) | ((uint32_t)b[du + 1] << 16) |
                     ((uint32_t)b[du + 2] << 8) | b[du + 3];
    }
    return r;
}

// Build a fragmented mux and add a configured HEVC track; return mux + track id.
// Caller drives put_sample/close. `mem` must outlive the mux.
MP4E_mux_t *open_with_track(MemFile *mem, int *track_id_out) {
    MP4E_mux_t *mux = MP4E_open(0 /*sequential*/, 1 /*fragmentation*/, mem, mem_write);
    REQUIRE(mux != nullptr);

    MP4E_track_t tr;
    std::memset(&tr, 0, sizeof(tr));
    tr.track_media_kind = e_video;
    tr.language[0] = 'u'; tr.language[1] = 'n'; tr.language[2] = 'd'; tr.language[3] = 0;
    tr.object_type_indication = MP4_OBJECT_TYPE_HEVC;
    tr.time_scale = 90000;
    tr.default_duration = 0;
    tr.u.v.width = 1920; tr.u.v.height = 1080;
    int track_id = MP4E_add_track(mux, &tr);
    REQUIRE(track_id >= 0);

    const uint8_t vps[] = {0x40, 0x01, 0x0c};
    const uint8_t sps[] = {0x42, 0x01, 0x01};
    const uint8_t pps[] = {0x44, 0x01, 0xc0};
    REQUIRE(MP4E_set_vps(mux, track_id, vps, sizeof(vps)) == MP4E_STATUS_OK);
    REQUIRE(MP4E_set_sps(mux, track_id, sps, sizeof(sps)) == MP4E_STATUS_OK);
    REQUIRE(MP4E_set_pps(mux, track_id, pps, sizeof(pps)) == MP4E_STATUS_OK);

    *track_id_out = track_id;
    return mux;
}

// Mux `durations` (90 kHz units) as fragments and return the finished buffer.
std::vector<uint8_t> mux_fragments(const std::vector<int> &durations) {
    MemFile mem;
    int track_id = -1;
    MP4E_mux_t *mux = open_with_track(&mem, &track_id);

    const uint8_t frame[] = {0, 0, 0, 1, 0x26, 0x01, 0xde, 0xad};
    for (int d : durations)
        REQUIRE(MP4E_put_sample(mux, track_id, frame, sizeof(frame), d,
                                MP4E_SAMPLE_RANDOM_ACCESS) == MP4E_STATUS_OK);

    REQUIRE(MP4E_close(mux) == MP4E_STATUS_OK);
    return mem.buf;
}

} // namespace

TEST_CASE("fragmented moov reports summed duration in mvhd", "[dvr][moov]") {
    // Variable per-frame durations (90 kHz). Summed total = 18000 => 0.2 s.
    // Note 5 frames * any single duration != 18000, so this also proves the
    // total is summed rather than fragments_count * one duration.
    const std::vector<int> durations = {1500, 3000, 4500, 3000, 6000};
    const uint64_t total_90k = 18000;

    auto buf = mux_fragments(durations);

    int64_t moov = find_box(buf, FOUR_CHAR_INT('m', 'o', 'o', 'v'), 0, buf.size());
    REQUIRE(moov >= 0);

    int64_t mvhd = find_box(buf, FOUR_CHAR_INT('m', 'v', 'h', 'd'), 0, buf.size());
    REQUIRE(mvhd >= 0);
    // mvhd v0 payload: creation(4)+modification(4)+timescale(4)+duration(4)
    DurField mv = read_mvhd_like(buf, mvhd, /*ts*/ 8, /*dur*/ 12);
    REQUIRE(mv.timescale > 0);
    // Expected duration in the movie timescale.
    uint64_t expected = total_90k * mv.timescale / 90000;
    REQUIRE(mv.duration == expected);
}

TEST_CASE("fragmented moov reports summed duration in mdhd", "[dvr][moov]") {
    const std::vector<int> durations = {1500, 3000, 4500, 3000, 6000};
    const uint64_t total_90k = 18000;

    auto buf = mux_fragments(durations);

    int64_t mdhd = find_box(buf, FOUR_CHAR_INT('m', 'd', 'h', 'd'), 0, buf.size());
    REQUIRE(mdhd >= 0);
    // mdhd v0 payload: creation(4)+modification(4)+timescale(4)+duration(4)
    DurField md = read_mvhd_like(buf, mdhd, /*ts*/ 8, /*dur*/ 12);
    REQUIRE(md.timescale == 90000);
    REQUIRE(md.duration == total_90k);
}

TEST_CASE("closing a fragmented mux with no frames does not crash or patch",
          "[dvr][moov]") {
    // A recording started and stopped before any frame: the moov was never
    // written (it is emitted before frame 1), so close() must not attempt a
    // back-patch. It should simply succeed and write no moov.
    MemFile mem;
    int track_id = -1;
    MP4E_mux_t *mux = open_with_track(&mem, &track_id);

    REQUIRE(MP4E_close(mux) == MP4E_STATUS_OK);

    int64_t moov = find_box(mem.buf, FOUR_CHAR_INT('m', 'o', 'o', 'v'), 0,
                            mem.buf.size());
    REQUIRE(moov < 0); // no moov for a frameless fragmented file
}
