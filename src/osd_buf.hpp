#ifndef OSD_BUF_HPP
#define OSD_BUF_HPP

// Picks the OSD buffer the Cairo refresh path paints next.
//
// `cur` is osd_buf_switch — the buffer most recently queued for scanout — and
// can be ANY index in [0, count): the LVGL flush callback parks it on 1 or 2
// (its two draw targets), so the refresh path must not assume a 0/1 ping-pong.
// The result must always be in [0, count) and never equal `cur`, which may
// still be on screen.
inline unsigned osd_next_paint_buf(unsigned cur, unsigned count) {
    return (cur + 1) % count;
}

#endif // OSD_BUF_HPP
