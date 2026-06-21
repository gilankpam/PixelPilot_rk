#ifndef FRAME_PROCESSOR_H
#define FRAME_PROCESSOR_H

#include <stdint.h>
#include <atomic>
#include <condition_variable>
#include <mutex>

#include <rockchip/rk_mpi.h>

#include "mpp_encoder.h"
#include "frame_colorcorrect.h"
#include "dvr_fps_cap.h"

// ---------------------------------------------------------------------------
// FrameProcessor: feeds MppEncoder at variable fps capped to cap_fps.
//
//  Single internal thread: receives decoded frames, performs
//  copy/resize/color-correction/OSD-blend, and pushes directly to the
//  encoder.  Frames that arrive faster than the cap are dropped.
// ---------------------------------------------------------------------------

struct FrameProcFrame {
    MppBuffer      buffer     = nullptr;
    uint32_t       width      = 0;
    uint32_t       height     = 0;
    uint32_t       hor_stride = 0;
    uint32_t       ver_stride = 0;
    MppFrameFormat fmt        = MPP_FMT_YUV420SP;
    void release() {
        if (buffer) { mpp_buffer_put(buffer); buffer = nullptr; }
    }
};

class FrameProcessor {
public:
    FrameProcessor(MppEncoder *enc, int cap_fps, uint32_t enc_w, uint32_t enc_h);
    ~FrameProcessor();

    // Called from decoder thread: update the latest available frame.
    void push_latest(MppBuffer buf, uint32_t w, uint32_t h,
                     uint32_t hs, uint32_t vs, MppFrameFormat fmt);

    void shutdown();

    // Enable GPU color correction using the DRM gamma formula y = clamp((x+offset)*gain, 0, 1).
    // Safe to call from any thread.  drm_fd is used to create the GBM/EGL context (lazy).
    void set_color_correction(float gain, float offset, int drm_fd);

    // Enable/disable color correction at runtime without changing the stored params.
    // Thread-safe: safe to toggle from the UI thread while the pacer is running.
    void set_color_correction_enabled(bool on) {
        color_correct_.store(on, std::memory_order_relaxed);
    }

    // Called from the frame thread before freeing the decoder buffer group
    // (on resolution change).  Releases any pending decoder buffer ref and
    // waits for any in-flight copy to finish so the group can be freed safely.
    void drain_decoder_refs();

    // Called from the OSD thread each time a new OSD frame is ready.
    // prime_fd  — DMA-buf fd of the OSD modeset_buf (BGRA/ARGB8888)
    // w, h      — OSD pixel dimensions
    // stride_px — row stride in pixels (= buf->stride / 4)
    void set_osd_blend(int prime_fd, uint32_t w, uint32_t h, uint32_t stride_px);

    static void *__THREAD__(void *p);

private:
    void process_loop();

    MppEncoder            *encoder;
    uint32_t               enc_w_ = 0, enc_h_ = 0;
    int                    cap_fps_ = 0;
    std::atomic<bool>     running{true};
    std::mutex              mtx;       // guards pending (shared with frame/decoder thread)
    std::condition_variable cv_;       // signalled by push_latest(); processor waits here
    std::mutex              copy_mtx_; // held by processor while it uses a decoder buffer
    FrameProcFrame     pending;   // latest from decoder (shared with decoder thread)

    // Only accessed from the processor thread — no mutex needed:
    MppBufferGroup    hold_grp  = nullptr;  // our own DRM buffer pool
    MppBuffer         proc_copy_  = nullptr;  // processor's working buffer
    MppBuffer         blend_rgba_ = nullptr;  // BGRA intermediate for OSD compositing
    FrameProcFrame     proc_meta_;              // metadata being built by processor

    // OSD blend — shared between OSD thread (writer) and processor thread (reader)
    struct OsdInfo {
        int      prime_fd{-1};
        uint32_t width{0}, height{0}, stride_px{0};
    };
    std::mutex  osd_mtx_;
    OsdInfo     osd_info_;      // latest OSD frame descriptor

    // Color correction — lazy-initialized on the processor thread on first frame.
    // Written by UI thread (set_color_correction / set_color_correction_enabled),
    // read by processor thread — must be atomic.
    std::atomic<bool>  color_correct_{false};
    bool               cc_init_done_{false};   // only attempt init once
    uint32_t           cc_width_{0}, cc_height_{0}; // dimensions at last init
    float              cc_gain_{1.f}, cc_offset_{0.f};
    int                cc_drm_fd_{-1};
    FrameColorCorrect  color_gl_;
};

#endif // FRAME_PROCESSOR_H
