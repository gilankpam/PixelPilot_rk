#include <cstddef>
#include <cstdint>
#include "hevc_depayloader.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    HevcDepayloader d([](const uint8_t*, size_t) {});
    // Derive marker + ts from the first bytes, feed the rest as a payload.
    bool marker = size ? (data[0] & 1) : false;
    uint32_t ts = size >= 5 ? (uint32_t(data[1])<<24 | uint32_t(data[2])<<16 |
                               uint32_t(data[3])<<8  | data[4]) : 0;
    const uint8_t* payload = size > 5 ? data + 5 : data;
    size_t plen = size > 5 ? size - 5 : 0;
    d.on_payload(payload, plen, marker, ts);
    return 0;
}
