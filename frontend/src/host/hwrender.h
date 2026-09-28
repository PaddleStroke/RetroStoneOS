/*
 * hwrender.h - libretro HW rendering (RETRO_ENVIRONMENT_SET_HW_RENDER) for
 * the GLES2 N64 cores: EGL on GBM, Mesa lima.
 *
 * Nothing here runs unless a core asks for HW rendering: libEGL,
 * libGLESv2 and libgbm are dlopen()ed at that moment (the host binary does
 * not link Mesa), and everything is torn down with the game.
 *
 * The core always renders into our FBO (colour texture + depth/stencil
 * renderbuffer). Two ways to show it:
 *  - zero-copy (preferred): GBM device on a dup of the KMS fd (Mesa kmsro:
 *    lima renders into scanout-capable buffers), a gbm_surface + EGL window
 *    surface; each frame the FBO texture is drawn into the window with one
 *    quad (this is where bottom_left_origin is honoured: the plane cannot
 *    flip), eglSwapBuffers, the front BO is wrapped in a DRM FB (cached per
 *    BO) and handed to display_present_fb();
 *  - readback (fallback, and headless / OSD frames): glReadPixels of the
 *    FBO, flipped and swizzled to XRGB8888 for display_present_frame().
 */
#ifndef RSOS_HOST_HWRENDER_H
#define RSOS_HOST_HWRENDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../third_party/libretro/libretro.h"

/* SET_HW_RENDER: checks the context type, loads the GL libraries, fills
 * get_current_framebuffer/get_proc_address. */
bool hwr_env_set(struct retro_hw_render_callback *cb);
/* GET_PREFERRED_HW_RENDER */
unsigned hwr_preferred(void);
/*
 * After retro_load_game(): creates the context and the FBO (max size),
 * makes it current. kms_fd >= 0 (display_get_drm_fd()) tries the zero-copy
 * setup first; on any failure (or kms_fd < 0) it uses the lima render node
 * with a surfaceless context (readback only). Returns 0 or -errno (msg set).
 * The caller calls cb->context_reset() on success.
 */
int hwr_init(unsigned max_w, unsigned max_h, int kms_fd, char *msg, size_t n);

/* Zero-copy present results. */
#define HWR_SHOWN     0   /* the display owns the BO until it is released */
#define HWR_FALLBACK  1   /* use hwr_readback() + display_present_frame() */
/*
 * Zero-copy present of the w x h frame: timeout_ms as display_present_fb()
 * (-1 = wait for the previous flip, i.e. vsync pacing; 0 = don't wait).
 * Returns HWR_SHOWN, HWR_FALLBACK (no zero-copy, or the plane refused the
 * FB: zero-copy is then turned off for the rest of the game), or a
 * negative errno when the frame was simply not shown (-EBUSY, -EAGAIN...).
 */
int hwr_present(unsigned w, unsigned h, int timeout_ms);
bool hwr_zero_copy(void);

/* Reads the *pw x *ph frame back: returns XRGB8888 top-down pixels. The
 * size is clamped to the render target and *pw, *ph are updated: the pitch
 * is the returned *pw * 4 (review F-M5: callers used the unclamped size). */
const void *hwr_readback(unsigned *pw, unsigned *ph);

/* Cumulative timing of the GLES path (microseconds, counts). */
struct hwr_timing {
	uint64_t swap_us;      /* quad blit + eglSwapBuffers + lock front buffer */
	uint64_t present_us;   /* display_present_fb(), including the flip wait */
	uint64_t readback_us;  /* glReadPixels + swizzle */
	uint64_t swaps, readbacks;
};
void hwr_get_timing(struct hwr_timing *t);
/*
 * Points Mesa's on-disk shader cache at dir (created if missing), unless
 * MESA_SHADER_CACHE_DIR is already set; NULL/"" = leave Mesa alone. Called
 * by hwr_env_set() and by the host before the core opens (GLES2 cores), in
 * any case before an EGL display exists. Once per process.
 */
void hwr_shader_cache_env(const char *dir);

/* Call after display_shutdown() (the display releases the BOs it holds). */
void hwr_deinit(void);
bool hwr_active(void);

#endif
