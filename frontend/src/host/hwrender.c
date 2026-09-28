/*
 * hwrender.c - see hwrender.h and docs/host-design.md section 13.
 */
#include "hwrender.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "glprobe.h"
#include "host_internal.h"
#include "hutil.h"
#include "../i18n/i18n.h"

/* Timing of the GLES path (perf.h counters), for both builds. */
static struct hwr_timing T;

void hwr_get_timing(struct hwr_timing *t)
{
	*t = T;
}

/*
 * Mesa's on-disk shader cache (docs/host-design.md §13.4). Without it every
 * game session compiles every shader and lima shader variant again, on the
 * frame that first uses it (hundreds of ms on the A7). Mesa picks
 * MESA_SHADER_CACHE_DIR, else $XDG_CACHE_HOME, else $HOME/.cache: on the
 * device HOME is "/" (read-only root), and the hardware log showed
 * "Failed to create //.cache for shader cache (Read-only file system)---disabling".
 * Database cache type: a few files that pack the entries (the multi-file
 * default makes one file per entry, each taking a whole exFAT cluster).
 * Must run before the EGL display is initialised; the game process is
 * single-GL, so the environment is ours.
 */
void hwr_shader_cache_env(const char *dir)
{
	static bool done;
	const char *have = getenv("MESA_SHADER_CACHE_DIR");

	if (done || !dir || !*dir)
		return;
	done = true;
	if (have && *have) {
		hlog(HLOG_INFO, "shader cache: MESA_SHADER_CACHE_DIR=%s (from the environment)", have);
		return;
	}
	if (hmkdir_p(dir, 0755) < 0 || access(dir, W_OK) != 0) {
		hlog(HLOG_WARN, "shader cache: %s is not writable (%s): Mesa compiles every shader each session",
		     dir, strerror(errno));
		return;
	}
	setenv("MESA_SHADER_CACHE_DIR", dir, 1);
	setenv("MESA_SHADER_CACHE_MAX_SIZE", "64M", 0);
	setenv("MESA_DISK_CACHE_DATABASE", "1", 0);
	setenv("MESA_DISK_CACHE_DATABASE_NUM_PARTS", "4", 0);
	/* hits/misses on stderr (game.log) when the GL screen goes away */
	setenv("MESA_SHADER_CACHE_SHOW_STATS", "true", 0);
	hlog(HLOG_INFO, "shader cache: %s (Mesa database, max 64 MB)", dir);
}

#if defined(__has_include)
#if __has_include(<EGL/egl.h>) && __has_include(<GLES2/gl2.h>)
#define HAVE_GLES_HEADERS 1
#endif
#endif

#ifdef HAVE_GLES_HEADERS

#define EGL_NO_X11 1
#define EGL_EGLEXT_PROTOTYPES 0
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif
#ifndef GL_DEPTH24_STENCIL8_OES
#define GL_DEPTH24_STENCIL8_OES 0x88F0
#endif

/*
 * The few libgbm entry points we need (stable ABI, dlopen()ed; no gbm.h
 * needed at build time).
 */
struct gbm_device;
struct gbm_surface;
struct gbm_bo;
union gbm_bo_handle_u {
	void *ptr;
	int32_t s32;
	uint32_t u32;
	int64_t s64;
	uint64_t u64;
};
#define RSOS_GBM_FORMAT_XRGB8888 0x34325258u /* 'XR24' */
#define RSOS_GBM_BO_USE_SCANOUT (1u << 0)
#define RSOS_GBM_BO_USE_RENDERING (1u << 2)

/* A DRM framebuffer attached to a GBM BO (gbm_bo_set_user_data). */
struct bo_fb {
	uint32_t fb_id;
	int fd;
};

static struct {
	bool active;
	bool zero_copy;
	struct retro_hw_render_callback *cb;  /* = &cbcopy */
	struct retro_hw_render_callback cbcopy;
	void *egl_lib, *gles_lib, *gbm_lib;
	int fd;                    /* render node, or our dup of the KMS fd */
	struct gbm_device *gbm;
	struct gbm_surface *gsurf;
	EGLDisplay dpy;
	EGLContext ctx;
	EGLSurface surf;
	GLuint fbo, tex, rb_depth, rb_stencil;
	GLuint prog, vs, fs;
	GLint u_tex;
	unsigned w, h;
	uint8_t *rgba;
	uint32_t *xrgb;
	unsigned presented, fallbacks;

	/* EGL */
	PFNEGLGETPROCADDRESSPROC GetProcAddress;
	EGLDisplay (*GetDisplay)(EGLNativeDisplayType);
	EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *);
	EGLBoolean (*Terminate)(EGLDisplay);
	EGLBoolean (*BindAPI)(EGLenum);
	EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
	EGLBoolean (*GetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);
	EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
	EGLBoolean (*DestroyContext)(EGLDisplay, EGLContext);
	EGLSurface (*CreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
	EGLBoolean (*DestroySurface)(EGLDisplay, EGLSurface);
	EGLBoolean (*SwapBuffers)(EGLDisplay, EGLSurface);
	EGLBoolean (*SwapInterval)(EGLDisplay, EGLint);
	EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
	EGLint (*GetError)(void);
	const char *(*QueryString)(EGLDisplay, EGLint);
	/* GBM */
	struct gbm_device *(*gbm_create_device)(int);
	void (*gbm_device_destroy)(struct gbm_device *);
	struct gbm_surface *(*gbm_surface_create)(struct gbm_device *, uint32_t, uint32_t, uint32_t, uint32_t);
	void (*gbm_surface_destroy)(struct gbm_surface *);
	struct gbm_bo *(*gbm_surface_lock_front_buffer)(struct gbm_surface *);
	void (*gbm_surface_release_buffer)(struct gbm_surface *, struct gbm_bo *);
	uint32_t (*gbm_bo_get_width)(struct gbm_bo *);
	uint32_t (*gbm_bo_get_height)(struct gbm_bo *);
	uint32_t (*gbm_bo_get_stride)(struct gbm_bo *);
	union gbm_bo_handle_u (*gbm_bo_get_handle)(struct gbm_bo *);
	void (*gbm_bo_set_user_data)(struct gbm_bo *, void *, void (*)(struct gbm_bo *, void *));
	void *(*gbm_bo_get_user_data)(struct gbm_bo *);
	/* GLES */
	void (*GenTextures)(GLsizei, GLuint *);
	void (*DeleteTextures)(GLsizei, const GLuint *);
	void (*BindTexture)(GLenum, GLuint);
	void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
	void (*TexParameteri)(GLenum, GLenum, GLint);
	void (*GenFramebuffers)(GLsizei, GLuint *);
	void (*DeleteFramebuffers)(GLsizei, const GLuint *);
	void (*BindFramebuffer)(GLenum, GLuint);
	void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
	void (*GenRenderbuffers)(GLsizei, GLuint *);
	void (*DeleteRenderbuffers)(GLsizei, const GLuint *);
	void (*BindRenderbuffer)(GLenum, GLuint);
	void (*RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
	void (*FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
	GLenum (*CheckFramebufferStatus)(GLenum);
	void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
	void (*PixelStorei)(GLenum, GLint);
	const GLubyte *(*GetString)(GLenum);
	void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
	void (*Clear)(GLbitfield);
	GLuint (*CreateShader)(GLenum);
	void (*DeleteShader)(GLuint);
	void (*ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
	void (*CompileShader)(GLuint);
	void (*GetShaderiv)(GLuint, GLenum, GLint *);
	GLuint (*CreateProgram)(void);
	void (*DeleteProgram)(GLuint);
	void (*AttachShader)(GLuint, GLuint);
	void (*BindAttribLocation)(GLuint, GLuint, const GLchar *);
	void (*LinkProgram)(GLuint);
	void (*GetProgramiv)(GLuint, GLenum, GLint *);
	void (*UseProgram)(GLuint);
	GLint (*GetUniformLocation)(GLuint, const GLchar *);
	void (*Uniform1i)(GLint, GLint);
	void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
	void (*EnableVertexAttribArray)(GLuint);
	void (*DisableVertexAttribArray)(GLuint);
	void (*GetVertexAttribiv)(GLuint, GLenum, GLint *);
	void (*DrawArrays)(GLenum, GLint, GLsizei);
	void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
	void (*ActiveTexture)(GLenum);
	void (*Enable)(GLenum);
	void (*Disable)(GLenum);
	GLboolean (*IsEnabled)(GLenum);
	void (*GetIntegerv)(GLenum, GLint *);
	void (*BindBuffer)(GLenum, GLuint);
	void (*ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
} G = { .fd = -1 };

#define LOAD(lib, field, name)                                   \
	do {                                                     \
		*(void **)&G.field = dlsym(lib, name);           \
		if (!G.field) {                                  \
			hlog(HLOG_ERROR, "hw render: %s missing", name); \
			return false;                            \
		}                                                \
	} while (0)

static bool load_libs(void)
{
	if (G.egl_lib && G.gles_lib && G.gbm_lib && G.Viewport)
		return true;
	G.egl_lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
	G.gles_lib = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
	G.gbm_lib = dlopen("libgbm.so.1", RTLD_NOW | RTLD_GLOBAL);
	if (!G.egl_lib || !G.gles_lib || !G.gbm_lib) {
		hlog(HLOG_ERROR, "hw render: cannot load EGL/GLESv2/gbm: %s", dlerror());
		return false;
	}
	glprobe_bind(G.gles_lib);
	LOAD(G.egl_lib, GetProcAddress, "eglGetProcAddress");
	LOAD(G.egl_lib, GetDisplay, "eglGetDisplay");
	LOAD(G.egl_lib, Initialize, "eglInitialize");
	LOAD(G.egl_lib, Terminate, "eglTerminate");
	LOAD(G.egl_lib, BindAPI, "eglBindAPI");
	LOAD(G.egl_lib, ChooseConfig, "eglChooseConfig");
	LOAD(G.egl_lib, GetConfigAttrib, "eglGetConfigAttrib");
	LOAD(G.egl_lib, CreateContext, "eglCreateContext");
	LOAD(G.egl_lib, DestroyContext, "eglDestroyContext");
	LOAD(G.egl_lib, CreateWindowSurface, "eglCreateWindowSurface");
	LOAD(G.egl_lib, DestroySurface, "eglDestroySurface");
	LOAD(G.egl_lib, SwapBuffers, "eglSwapBuffers");
	LOAD(G.egl_lib, SwapInterval, "eglSwapInterval");
	LOAD(G.egl_lib, MakeCurrent, "eglMakeCurrent");
	LOAD(G.egl_lib, GetError, "eglGetError");
	LOAD(G.egl_lib, QueryString, "eglQueryString");
	LOAD(G.gbm_lib, gbm_create_device, "gbm_create_device");
	LOAD(G.gbm_lib, gbm_device_destroy, "gbm_device_destroy");
	LOAD(G.gbm_lib, gbm_surface_create, "gbm_surface_create");
	LOAD(G.gbm_lib, gbm_surface_destroy, "gbm_surface_destroy");
	LOAD(G.gbm_lib, gbm_surface_lock_front_buffer, "gbm_surface_lock_front_buffer");
	LOAD(G.gbm_lib, gbm_surface_release_buffer, "gbm_surface_release_buffer");
	LOAD(G.gbm_lib, gbm_bo_get_width, "gbm_bo_get_width");
	LOAD(G.gbm_lib, gbm_bo_get_height, "gbm_bo_get_height");
	LOAD(G.gbm_lib, gbm_bo_get_stride, "gbm_bo_get_stride");
	LOAD(G.gbm_lib, gbm_bo_get_handle, "gbm_bo_get_handle");
	LOAD(G.gbm_lib, gbm_bo_set_user_data, "gbm_bo_set_user_data");
	LOAD(G.gbm_lib, gbm_bo_get_user_data, "gbm_bo_get_user_data");
	LOAD(G.gles_lib, GenTextures, "glGenTextures");
	LOAD(G.gles_lib, DeleteTextures, "glDeleteTextures");
	LOAD(G.gles_lib, BindTexture, "glBindTexture");
	LOAD(G.gles_lib, TexImage2D, "glTexImage2D");
	LOAD(G.gles_lib, TexParameteri, "glTexParameteri");
	LOAD(G.gles_lib, GenFramebuffers, "glGenFramebuffers");
	LOAD(G.gles_lib, DeleteFramebuffers, "glDeleteFramebuffers");
	LOAD(G.gles_lib, BindFramebuffer, "glBindFramebuffer");
	LOAD(G.gles_lib, FramebufferTexture2D, "glFramebufferTexture2D");
	LOAD(G.gles_lib, GenRenderbuffers, "glGenRenderbuffers");
	LOAD(G.gles_lib, DeleteRenderbuffers, "glDeleteRenderbuffers");
	LOAD(G.gles_lib, BindRenderbuffer, "glBindRenderbuffer");
	LOAD(G.gles_lib, RenderbufferStorage, "glRenderbufferStorage");
	LOAD(G.gles_lib, FramebufferRenderbuffer, "glFramebufferRenderbuffer");
	LOAD(G.gles_lib, CheckFramebufferStatus, "glCheckFramebufferStatus");
	LOAD(G.gles_lib, ReadPixels, "glReadPixels");
	LOAD(G.gles_lib, PixelStorei, "glPixelStorei");
	LOAD(G.gles_lib, GetString, "glGetString");
	LOAD(G.gles_lib, ClearColor, "glClearColor");
	LOAD(G.gles_lib, Clear, "glClear");
	LOAD(G.gles_lib, CreateShader, "glCreateShader");
	LOAD(G.gles_lib, DeleteShader, "glDeleteShader");
	LOAD(G.gles_lib, ShaderSource, "glShaderSource");
	LOAD(G.gles_lib, CompileShader, "glCompileShader");
	LOAD(G.gles_lib, GetShaderiv, "glGetShaderiv");
	LOAD(G.gles_lib, CreateProgram, "glCreateProgram");
	LOAD(G.gles_lib, DeleteProgram, "glDeleteProgram");
	LOAD(G.gles_lib, AttachShader, "glAttachShader");
	LOAD(G.gles_lib, BindAttribLocation, "glBindAttribLocation");
	LOAD(G.gles_lib, LinkProgram, "glLinkProgram");
	LOAD(G.gles_lib, GetProgramiv, "glGetProgramiv");
	LOAD(G.gles_lib, UseProgram, "glUseProgram");
	LOAD(G.gles_lib, GetUniformLocation, "glGetUniformLocation");
	LOAD(G.gles_lib, Uniform1i, "glUniform1i");
	LOAD(G.gles_lib, VertexAttribPointer, "glVertexAttribPointer");
	LOAD(G.gles_lib, EnableVertexAttribArray, "glEnableVertexAttribArray");
	LOAD(G.gles_lib, DisableVertexAttribArray, "glDisableVertexAttribArray");
	LOAD(G.gles_lib, GetVertexAttribiv, "glGetVertexAttribiv");
	LOAD(G.gles_lib, DrawArrays, "glDrawArrays");
	LOAD(G.gles_lib, Viewport, "glViewport");
	LOAD(G.gles_lib, ActiveTexture, "glActiveTexture");
	LOAD(G.gles_lib, Enable, "glEnable");
	LOAD(G.gles_lib, Disable, "glDisable");
	LOAD(G.gles_lib, IsEnabled, "glIsEnabled");
	LOAD(G.gles_lib, GetIntegerv, "glGetIntegerv");
	LOAD(G.gles_lib, BindBuffer, "glBindBuffer");
	LOAD(G.gles_lib, ColorMask, "glColorMask");
	return true;
}

static uintptr_t get_current_framebuffer(void)
{
	return G.fbo;
}

static retro_proc_address_t get_proc_address(const char *sym)
{
	void *p = glprobe_wrap(sym);      /* the timed wrappers (glprobe.h) */

	if (!p && G.GetProcAddress)
		p = (void *)G.GetProcAddress(sym);
	if (!p && G.gles_lib)
		p = dlsym(G.gles_lib, sym);
	if (!p && G.egl_lib)
		p = dlsym(G.egl_lib, sym);
	return (retro_proc_address_t)p;
}

unsigned hwr_preferred(void)
{
	return RETRO_HW_CONTEXT_OPENGLES2;
}

bool hwr_env_set(struct retro_hw_render_callback *cb)
{
	if (!cb)
		return false;
	switch (cb->context_type) {
	case RETRO_HW_CONTEXT_OPENGLES2:
		break;
	case RETRO_HW_CONTEXT_OPENGLES_VERSION:
		if (cb->version_major == 2)
			break;
		/* fallthrough */
	default:
		hlog(HLOG_WARN, "hw render: context type %d (v%u.%u) not supported (lima: GLES 2.0 only)",
		     (int)cb->context_type, cb->version_major, cb->version_minor);
		return false;
	}
	if (!load_libs())
		return false;
	hwr_shader_cache_env(H.cfg.shader_cache_dir);  /* before any EGL display exists */
	cb->get_current_framebuffer = get_current_framebuffer;
	cb->get_proc_address = get_proc_address;
	G.cbcopy = *cb;
	G.cb = &G.cbcopy;
	hlog(HLOG_INFO, "hw render: GLES2 requested (depth %d, stencil %d, bottom-left %d)",
	     cb->depth, cb->stencil, cb->bottom_left_origin);
	return true;
}

/* The lima render node (fallback: the first render node). */
static int open_render_node(void)
{
	int first = -1;

	for (int i = 128; i < 136; i++) {
		char path[32];
		int fd;
		drmVersionPtr v;

		snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		v = drmGetVersion(fd);
		if (v && v->name && !strcmp(v->name, "lima")) {
			drmFreeVersion(v);
			if (first >= 0)
				close(first);
			hlog(HLOG_INFO, "hw render: %s (lima)", path);
			return fd;
		}
		if (v)
			drmFreeVersion(v);
		if (first < 0)
			first = fd;
		else
			close(fd);
	}
	return first;
}

/* Tears down EGL/GBM (not the libraries). */
static void egl_teardown(void)
{
	if (G.dpy != EGL_NO_DISPLAY && G.dpy) {
		if (G.ctx != EGL_NO_CONTEXT && G.ctx) {
			if (G.fbo)
				G.DeleteFramebuffers(1, &G.fbo);
			if (G.tex)
				G.DeleteTextures(1, &G.tex);
			if (G.rb_depth)
				G.DeleteRenderbuffers(1, &G.rb_depth);
			if (G.rb_stencil)
				G.DeleteRenderbuffers(1, &G.rb_stencil);
			if (G.prog)
				G.DeleteProgram(G.prog);
			if (G.vs)
				G.DeleteShader(G.vs);
			if (G.fs)
				G.DeleteShader(G.fs);
			G.MakeCurrent(G.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
			G.DestroyContext(G.dpy, G.ctx);
		}
		if (G.surf != EGL_NO_SURFACE && G.surf)
			G.DestroySurface(G.dpy, G.surf);
		G.Terminate(G.dpy);
	}
	if (G.gsurf)
		G.gbm_surface_destroy(G.gsurf);  /* BO destroy callbacks remove the FBs */
	if (G.gbm)
		G.gbm_device_destroy(G.gbm);
	if (G.fd >= 0)
		close(G.fd);
	G.fbo = G.tex = G.rb_depth = G.rb_stencil = G.prog = G.vs = G.fs = 0;
	G.ctx = EGL_NO_CONTEXT;
	G.surf = EGL_NO_SURFACE;
	G.dpy = EGL_NO_DISPLAY;
	G.gsurf = NULL;
	G.gbm = NULL;
	G.fd = -1;
	G.zero_copy = false;
}

static EGLDisplay get_display(struct gbm_device *gbm)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC gpd =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)G.GetProcAddress("eglGetPlatformDisplayEXT");

	return gpd ? gpd(EGL_PLATFORM_GBM_KHR, gbm, NULL) : G.GetDisplay((EGLNativeDisplayType)gbm);
}

static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

/* Zero-copy: GBM on a dup of the KMS fd (kmsro), window surface. */
static bool setup_zero_copy(int kms_fd, unsigned w, unsigned h)
{
	static const EGLint attr[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0,
		EGL_NONE,
	};
	EGLConfig cfgs[64], cfg = NULL;
	EGLint n = 0, major, minor;

	/* Our own reference to the DRM file: the GEM handles stay valid after
	 * display_shutdown() closes the display's fd (same drm_file). */
	G.fd = fcntl(kms_fd, F_DUPFD_CLOEXEC, 3);
	if (G.fd < 0)
		return false;
	G.gbm = G.gbm_create_device(G.fd);
	if (!G.gbm)
		return false;
	G.dpy = get_display(G.gbm);
	if (G.dpy == EGL_NO_DISPLAY || !G.Initialize(G.dpy, &major, &minor))
		return false;
	G.BindAPI(EGL_OPENGL_ES_API);
	if (!G.ChooseConfig(G.dpy, attr, cfgs, 64, &n) || n < 1)
		return false;
	for (int i = 0; i < n && !cfg; i++) {
		EGLint vid = 0;

		if (G.GetConfigAttrib(G.dpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &vid) &&
		    (uint32_t)vid == RSOS_GBM_FORMAT_XRGB8888)
			cfg = cfgs[i];
	}
	if (!cfg)
		return false;
	G.gsurf = G.gbm_surface_create(G.gbm, w, h, RSOS_GBM_FORMAT_XRGB8888,
				       RSOS_GBM_BO_USE_SCANOUT | RSOS_GBM_BO_USE_RENDERING);
	if (!G.gsurf)
		return false;
	G.surf = G.CreateWindowSurface(G.dpy, cfg, (EGLNativeWindowType)G.gsurf, NULL);
	if (G.surf == EGL_NO_SURFACE)
		return false;
	G.ctx = G.CreateContext(G.dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	if (G.ctx == EGL_NO_CONTEXT || !G.MakeCurrent(G.dpy, G.surf, G.surf, G.ctx))
		return false;
	G.SwapInterval(G.dpy, 0); /* the display layer paces, not EGL */
	hlog(HLOG_INFO, "hw render: zero-copy (GBM on the KMS device, EGL %d.%d)", major, minor);
	return true;
}

/* Readback only: lima render node, surfaceless context. */
static bool setup_readback(void)
{
	static const EGLint attr[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 1, EGL_GREEN_SIZE, 1, EGL_BLUE_SIZE, 1,
		EGL_NONE,
	};
	EGLConfig cfg;
	EGLint major, minor, n = 0;
	const char *exts;

	G.fd = open_render_node();
	if (G.fd < 0)
		return false;
	G.gbm = G.gbm_create_device(G.fd);
	if (!G.gbm)
		return false;
	G.dpy = get_display(G.gbm);
	if (G.dpy == EGL_NO_DISPLAY || !G.Initialize(G.dpy, &major, &minor))
		return false;
	exts = G.QueryString(G.dpy, EGL_EXTENSIONS);
	if (!exts || !strstr(exts, "EGL_KHR_surfaceless_context"))
		hlog(HLOG_WARN, "EGL: no EGL_KHR_surfaceless_context");
	G.BindAPI(EGL_OPENGL_ES_API);
	if (!G.ChooseConfig(G.dpy, attr, &cfg, 1, &n) || n < 1)
		return false;
	G.ctx = G.CreateContext(G.dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	if (G.ctx == EGL_NO_CONTEXT || !G.MakeCurrent(G.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, G.ctx))
		return false;
	hlog(HLOG_INFO, "hw render: readback path (render node, surfaceless, EGL %d.%d)", major, minor);
	return true;
}

static GLuint compile(GLenum type, const char *src)
{
	GLuint s = G.CreateShader(type);
	GLint ok = 0;

	G.ShaderSource(s, 1, &src, NULL);
	G.CompileShader(s);
	G.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		G.DeleteShader(s);
		return 0;
	}
	return s;
}

static bool setup_blit_program(void)
{
	static const char *vs =
		"attribute vec2 a_pos;\n"
		"attribute vec2 a_uv;\n"
		"varying vec2 v_uv;\n"
		"void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
	static const char *fs =
		"precision mediump float;\n"
		"varying vec2 v_uv;\n"
		"uniform sampler2D u_tex;\n"
		"void main() { gl_FragColor = vec4(texture2D(u_tex, v_uv).rgb, 1.0); }\n";
	GLint ok = 0;

	G.vs = compile(GL_VERTEX_SHADER, vs);
	G.fs = compile(GL_FRAGMENT_SHADER, fs);
	if (!G.vs || !G.fs)
		return false;
	G.prog = G.CreateProgram();
	G.AttachShader(G.prog, G.vs);
	G.AttachShader(G.prog, G.fs);
	G.BindAttribLocation(G.prog, 0, "a_pos");
	G.BindAttribLocation(G.prog, 1, "a_uv");
	G.LinkProgram(G.prog);
	G.GetProgramiv(G.prog, GL_LINK_STATUS, &ok);
	if (!ok)
		return false;
	G.u_tex = G.GetUniformLocation(G.prog, "u_tex");
	return true;
}

static bool setup_fbo(unsigned max_w, unsigned max_h, char *msg, size_t n)
{
	GLenum st;

	G.GenTextures(1, &G.tex);
	G.BindTexture(GL_TEXTURE_2D, G.tex);
	G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)max_w, (GLsizei)max_h, 0, GL_RGBA,
		     GL_UNSIGNED_BYTE, NULL);
	G.BindTexture(GL_TEXTURE_2D, 0);
	G.GenFramebuffers(1, &G.fbo);
	G.BindFramebuffer(GL_FRAMEBUFFER, G.fbo);
	G.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, G.tex, 0);
	if (G.cb->depth || G.cb->stencil) {
		const char *glext = (const char *)G.GetString(GL_EXTENSIONS);
		bool packed = glext && strstr(glext, "GL_OES_packed_depth_stencil");

		G.GenRenderbuffers(1, &G.rb_depth);
		G.BindRenderbuffer(GL_RENDERBUFFER, G.rb_depth);
		if (G.cb->stencil && packed) {
			G.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, (GLsizei)max_w, (GLsizei)max_h);
			G.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, G.rb_depth);
			G.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, G.rb_depth);
		} else {
			G.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, (GLsizei)max_w, (GLsizei)max_h);
			G.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, G.rb_depth);
			if (G.cb->stencil) {
				G.GenRenderbuffers(1, &G.rb_stencil);
				G.BindRenderbuffer(GL_RENDERBUFFER, G.rb_stencil);
				G.RenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, (GLsizei)max_w, (GLsizei)max_h);
				G.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, G.rb_stencil);
			}
		}
		G.BindRenderbuffer(GL_RENDERBUFFER, 0);
	}
	st = G.CheckFramebufferStatus(GL_FRAMEBUFFER);
	if (st != GL_FRAMEBUFFER_COMPLETE) {
		snprintf(msg, n, _("GPU init failed (framebuffer 0x%x)"), st);
		return false;
	}
	G.ClearColor(0, 0, 0, 1);
	G.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	return true;
}

int hwr_init(unsigned max_w, unsigned max_h, int kms_fd, char *msg, size_t n)
{
	if (!G.cb || !load_libs()) {
		snprintf(msg, n, "%s", _("This build cannot run GLES games"));
		return -ENOTSUP;
	}
	G.w = max_w;
	G.h = max_h;
	if (kms_fd >= 0 && setup_zero_copy(kms_fd, max_w, max_h) && setup_blit_program()) {
		G.zero_copy = true;
	} else {
		if (kms_fd >= 0)
			hlog(HLOG_WARN, "hw render: zero-copy setup failed (EGL 0x%x), using readback", G.GetError());
		egl_teardown();
		if (!setup_readback()) {
			snprintf(msg, n, "%s", _("No GPU found (lima)"));
			egl_teardown();
			return -ENODEV;
		}
	}
	hlog(HLOG_INFO, "GL: %s / %s", G.GetString(GL_RENDERER), G.GetString(GL_VERSION));
	if (!setup_fbo(max_w, max_h, msg, n)) {
		egl_teardown();
		return -EIO;
	}
	G.rgba = malloc((size_t)max_w * max_h * 4);
	G.xrgb = malloc((size_t)max_w * max_h * 4);
	if (!G.rgba || !G.xrgb) {
		hwr_deinit();
		return -ENOMEM;
	}
	G.active = true;
	hlog(HLOG_INFO, "hw render: FBO %ux%u ready", max_w, max_h);
	return 0;
}

bool hwr_zero_copy(void)
{
	return G.active && G.zero_copy;
}

/* ------------------------------------------------------ zero-copy */

static void bo_destroyed(struct gbm_bo *bo, void *data)
{
	struct bo_fb *f = data;

	(void)bo;
	if (f->fb_id)
		drmModeRmFB(f->fd, f->fb_id);
	free(f);
}

static uint32_t fb_for_bo(struct gbm_bo *bo)
{
	struct bo_fb *f = G.gbm_bo_get_user_data(bo);
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };

	if (f)
		return f->fb_id;
	f = calloc(1, sizeof(*f));
	if (!f)
		return 0;
	f->fd = G.fd;
	handles[0] = G.gbm_bo_get_handle(bo).u32;
	pitches[0] = G.gbm_bo_get_stride(bo);
	if (drmModeAddFB2(G.fd, G.gbm_bo_get_width(bo), G.gbm_bo_get_height(bo), DRM_FORMAT_XRGB8888,
			  handles, pitches, offsets, &f->fb_id, 0) != 0) {
		hlog(HLOG_ERROR, "hw render: drmModeAddFB2: %s", strerror(errno));
		free(f);
		return 0;
	}
	G.gbm_bo_set_user_data(bo, f, bo_destroyed);
	return f->fb_id;
}

static void release_bo(uint32_t fb_id, void *user)
{
	(void)fb_id;
	if (G.gsurf)
		G.gbm_surface_release_buffer(G.gsurf, user);
}

/*
 * Draws the w x h frame of the core's FBO into the top-left corner of the
 * window surface (the BO's src rect {0, 0, w, h}). GL's origin is
 * bottom-left and Mesa presents window surfaces upright, so the BO rows
 * [0, h) are GL rows [H - h, H). A bottom_left_origin frame is copied as
 * is; a top-left-origin frame is flipped by the texture coordinates.
 */
static void blit_to_window(unsigned w, unsigned h)
{
	float s1 = (float)w / (float)G.w, t1 = (float)h / (float)G.h;
	bool bl = G.cb->bottom_left_origin;
	const GLfloat pos[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	const GLfloat uv[8] = {
		0, bl ? 0 : t1, s1, bl ? 0 : t1,
		0, bl ? t1 : 0, s1, bl ? t1 : 0,
	};
	GLint prog, tex, active, abuf, vp[4], en0, en1;
	GLboolean depth, blend, scissor, cull, stencil;

	/* Save what a core may not re-apply itself. */
	G.GetIntegerv(GL_CURRENT_PROGRAM, &prog);
	G.GetIntegerv(GL_ACTIVE_TEXTURE, &active);
	G.ActiveTexture(GL_TEXTURE0);
	G.GetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
	G.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
	G.GetIntegerv(GL_VIEWPORT, vp);
	G.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &en0);
	G.GetVertexAttribiv(1, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &en1);
	depth = G.IsEnabled(GL_DEPTH_TEST);
	blend = G.IsEnabled(GL_BLEND);
	scissor = G.IsEnabled(GL_SCISSOR_TEST);
	cull = G.IsEnabled(GL_CULL_FACE);
	stencil = G.IsEnabled(GL_STENCIL_TEST);

	G.BindFramebuffer(GL_FRAMEBUFFER, 0);
	G.Disable(GL_DEPTH_TEST);
	G.Disable(GL_BLEND);
	G.Disable(GL_SCISSOR_TEST);
	G.Disable(GL_CULL_FACE);
	G.Disable(GL_STENCIL_TEST);
	G.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	G.Viewport(0, (GLint)(G.h - h), (GLsizei)w, (GLsizei)h);
	G.UseProgram(G.prog);
	G.BindTexture(GL_TEXTURE_2D, G.tex);
	G.Uniform1i(G.u_tex, 0);
	G.BindBuffer(GL_ARRAY_BUFFER, 0);
	G.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
	G.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
	G.EnableVertexAttribArray(0);
	G.EnableVertexAttribArray(1);
	G.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	/* Restore. */
	if (!en0)
		G.DisableVertexAttribArray(0);
	if (!en1)
		G.DisableVertexAttribArray(1);
	G.BindBuffer(GL_ARRAY_BUFFER, (GLuint)abuf);
	G.BindTexture(GL_TEXTURE_2D, (GLuint)tex);
	G.ActiveTexture((GLenum)active);
	G.UseProgram((GLuint)prog);
	G.Viewport(vp[0], vp[1], vp[2], vp[3]);
	if (depth)
		G.Enable(GL_DEPTH_TEST);
	if (blend)
		G.Enable(GL_BLEND);
	if (scissor)
		G.Enable(GL_SCISSOR_TEST);
	if (cull)
		G.Enable(GL_CULL_FACE);
	if (stencil)
		G.Enable(GL_STENCIL_TEST);
	G.BindFramebuffer(GL_FRAMEBUFFER, G.fbo);
}

int hwr_present(unsigned w, unsigned h, int timeout_ms)
{
	struct gbm_bo *bo;
	struct display_fb f;
	uint32_t fb_id;
	int64_t t0, t1;
	int r;

	if (!hwr_zero_copy())
		return HWR_FALLBACK;
	if (w > G.w)
		w = G.w;
	if (h > G.h)
		h = G.h;
	if (timeout_ms == 0 && display_flip_pending())
		return -EBUSY; /* audio-clock mode: skip before rendering anything */
	t0 = hnow_us();
	blit_to_window(w, h);
	if (!G.SwapBuffers(G.dpy, G.surf)) {
		hlog(HLOG_WARN, "hw render: eglSwapBuffers 0x%x: using readback", G.GetError());
		G.zero_copy = false;
		return HWR_FALLBACK;
	}
	bo = G.gbm_surface_lock_front_buffer(G.gsurf);
	t1 = hnow_us();
	T.swap_us += (uint64_t)(t1 - t0);
	T.swaps++;
	if (!bo)
		return -ENOMEM;
	fb_id = fb_for_bo(bo);
	if (!fb_id) {
		G.gbm_surface_release_buffer(G.gsurf, bo);
		G.zero_copy = false;
		return HWR_FALLBACK;
	}
	memset(&f, 0, sizeof(f));
	f.fb_id = fb_id;
	f.format = DRM_FORMAT_XRGB8888;
	f.width = (int)G.gbm_bo_get_width(bo);
	f.height = (int)G.gbm_bo_get_height(bo);
	f.src.x = 0;
	f.src.y = 0;
	f.src.w = (int)w;
	f.src.h = (int)h;
	f.release = release_bo;
	f.user = bo;
	r = display_present_fb(&f, timeout_ms);
	T.present_us += (uint64_t)(hnow_us() - t1);
	if (r == 0) {
		if (!G.presented++)
			hlog(HLOG_INFO, "hw render: first zero-copy frame %ux%u shown", w, h);
		return HWR_SHOWN;
	}
	/* Not shown: the BO is still ours. */
	G.gbm_surface_release_buffer(G.gsurf, bo);
	if (r == -EINVAL) {
		hlog(HLOG_WARN, "hw render: the plane refuses the GBM buffer: readback from now on");
		G.zero_copy = false;
		return HWR_FALLBACK;
	}
	return r;
}

/* ------------------------------------------------------- readback */

const void *hwr_readback(unsigned *pw, unsigned *ph)
{
	bool flip;
	int64_t t0 = hnow_us();
	unsigned w = *pw, h = *ph;

	if (!G.active || !w || !h)
		return NULL;
	if (w > G.w || h > G.h)
		hlog_once("hwr-clamp", "hw render: frame %ux%u larger than the render target %ux%u: cropped",
			  w, h, G.w, G.h);
	if (w > G.w)
		w = G.w;
	if (h > G.h)
		h = G.h;
	*pw = w;
	*ph = h;
	flip = G.cb->bottom_left_origin;
	G.BindFramebuffer(GL_FRAMEBUFFER, G.fbo);
	G.PixelStorei(GL_PACK_ALIGNMENT, 4);
	G.ReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, G.rgba);
	for (unsigned y = 0; y < h; y++) {
		const uint8_t *s = G.rgba + (size_t)(flip ? h - 1 - y : y) * w * 4;
		uint32_t *d = G.xrgb + (size_t)y * w;

		for (unsigned x = 0; x < w; x++, s += 4)
			d[x] = (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2];
	}
	T.readback_us += (uint64_t)(hnow_us() - t0);
	T.readbacks++;
	return G.xrgb;
}

void hwr_deinit(void)
{
	if (G.presented)
		hlog(HLOG_INFO, "hw render: %u zero-copy frames", G.presented);
	egl_teardown();
	free(G.rgba);
	free(G.xrgb);
	G.rgba = NULL;
	G.xrgb = NULL;
	G.active = false;
	/* The libraries stay loaded: the core still references their symbols
	 * until the process exits. */
}

bool hwr_active(void)
{
	return G.active;
}

#else /* !HAVE_GLES_HEADERS: this build has no GLES support. */

unsigned hwr_preferred(void)
{
	return RETRO_HW_CONTEXT_NONE;
}

bool hwr_env_set(struct retro_hw_render_callback *cb)
{
	(void)cb;
	hlog(HLOG_WARN, "hw render requested, but this build has no GLES support");
	return false;
}

int hwr_init(unsigned max_w, unsigned max_h, int kms_fd, char *msg, size_t n)
{
	(void)max_w;
	(void)max_h;
	(void)kms_fd;
	snprintf(msg, n, "%s", _("This build cannot run GLES games"));
	return -ENOTSUP;
}

int hwr_present(unsigned w, unsigned h, int timeout_ms)
{
	(void)w;
	(void)h;
	(void)timeout_ms;
	return HWR_FALLBACK;
}

bool hwr_zero_copy(void)
{
	return false;
}

const void *hwr_readback(unsigned *pw, unsigned *ph)
{
	(void)pw;
	(void)ph;
	return NULL;
}

void hwr_deinit(void)
{
}

bool hwr_active(void)
{
	return false;
}

#endif
