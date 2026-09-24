/* The SDL2 functions DOSBox-X calls, for a build with no display, input or
 * sound device: --enable-headless links this instead of libSDL2. Windows are
 * memory surfaces nobody shows, events are a queue only the emulator fills,
 * and audio, joysticks and the host clipboard are absent. SDL's headers
 * supply the types. */

#include "config.h"

#if C_HEADLESS

#include <SDL.h>
#include <SDL_syswm.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <deque>
#include <string>

/* Errors and hints */

static thread_local char error_text[256];

int SDL_SetError(SDL_PRINTF_FORMAT_STRING const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(error_text, sizeof(error_text), fmt, args);
    va_end(args);
    return -1;
}

const char *SDL_GetError(void) { return error_text; }

int SDL_Error(SDL_errorcode code) {
    return SDL_SetError(code == SDL_ENOMEM ? "Out of memory" : "SDL error %d", (int)code);
}

SDL_bool SDL_SetHint(const char *, const char *) { return SDL_TRUE; }
SDL_bool SDL_SetHintWithPriority(const char *, const char *, SDL_HintPriority) { return SDL_TRUE; }

void SDL_GetVersion(SDL_version *ver) { SDL_VERSION(ver); }

/* Every subsystem starts, as SDL's does with a driver that has no devices;
 * opening a device is what fails. */

int SDL_InitSubSystem(Uint32) { return 0; }
int SDL_Init(Uint32) { return 0; }
void SDL_QuitSubSystem(Uint32) {}
void SDL_Quit(void) {}

const char *SDL_GetCurrentVideoDriver(void) { return "headless"; }
const char *SDL_GetCurrentAudioDriver(void) { return NULL; }

/* libc */

void *SDL_malloc(size_t size) { return malloc(size); }
void *SDL_calloc(size_t n, size_t size) { return calloc(n, size); }
void *SDL_realloc(void *mem, size_t size) { return realloc(mem, size); }
void SDL_free(void *mem) { free(mem); }
void *SDL_memcpy(SDL_OUT_BYTECAP(len) void *dst, SDL_IN_BYTECAP(len) const void *src, size_t len) { return memcpy(dst, src, len); }
void *SDL_memset(SDL_OUT_BYTECAP(len) void *dst, int c, size_t len) { return memset(dst, c, len); }
int SDL_memcmp(const void *a, const void *b, size_t len) { return memcmp(a, b, len); }
void SDL_qsort(void *base, size_t n, size_t size, int (SDLCALL *compare)(const void *, const void *)) { qsort(base, n, size, compare); }

/* Time */

static Uint64 now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (Uint64)ts.tv_sec * 1000000000u + (Uint64)ts.tv_nsec;
}

static const Uint64 started_ns = now_ns();

Uint32 SDL_GetTicks(void) { return (Uint32)((now_ns() - started_ns) / 1000000u); }

void SDL_Delay(Uint32 ms) {
    struct timespec ts = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    while (nanosleep(&ts, &ts) != 0) {}
}

/* Threads */

struct SDL_Thread {
    pthread_t handle;
    SDL_ThreadFunction fn;
    void *data;
    int status;
};

static void *thread_main(void *arg) {
    SDL_Thread *thread = (SDL_Thread *)arg;
    thread->status = thread->fn(thread->data);
    return NULL;
}

SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *, void *data) {
    SDL_Thread *thread = new SDL_Thread{pthread_t(), fn, data, 0};
    if (pthread_create(&thread->handle, NULL, thread_main, thread) != 0) {
        delete thread;
        SDL_SetError("pthread_create failed");
        return NULL;
    }
    return thread;
}

void SDL_WaitThread(SDL_Thread *thread, int *status) {
    if (!thread) return;
    pthread_join(thread->handle, NULL);
    if (status) *status = thread->status;
    delete thread;
}

SDL_threadID SDL_ThreadID(void) {
    return (SDL_threadID)(uintptr_t)pthread_self();
}

SDL_threadID SDL_GetThreadID(SDL_Thread *thread) {
    if (!thread) return SDL_ThreadID();
    return (SDL_threadID)(uintptr_t)thread->handle;
}

struct SDL_mutex { pthread_mutex_t m; };
struct SDL_cond { pthread_cond_t c; };

SDL_mutex *SDL_CreateMutex(void) {
    SDL_mutex *mutex = new SDL_mutex;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);  /* SDL's mutexes are recursive */
    pthread_mutex_init(&mutex->m, &attr);
    pthread_mutexattr_destroy(&attr);
    return mutex;
}
void SDL_DestroyMutex(SDL_mutex *mutex) {
    if (!mutex) return;
    pthread_mutex_destroy(&mutex->m);
    delete mutex;
}
int SDL_LockMutex(SDL_mutex *mutex) { return mutex ? pthread_mutex_lock(&mutex->m) : 0; }
int SDL_UnlockMutex(SDL_mutex *mutex) { return mutex ? pthread_mutex_unlock(&mutex->m) : 0; }

SDL_cond *SDL_CreateCond(void) {
    SDL_cond *cond = new SDL_cond;
    pthread_cond_init(&cond->c, NULL);
    return cond;
}
int SDL_CondSignal(SDL_cond *cond) { return pthread_cond_signal(&cond->c); }
int SDL_CondWait(SDL_cond *cond, SDL_mutex *mutex) { return pthread_cond_wait(&cond->c, &mutex->m); }

/* macOS has no unnamed POSIX semaphores. */
struct SDL_semaphore {
    pthread_mutex_t m;
    pthread_cond_t c;
    Uint32 count;
};

SDL_sem *SDL_CreateSemaphore(Uint32 count) {
    SDL_sem *sem = new SDL_sem;
    pthread_mutex_init(&sem->m, NULL);
    pthread_cond_init(&sem->c, NULL);
    sem->count = count;
    return sem;
}
int SDL_SemPost(SDL_sem *sem) {
    pthread_mutex_lock(&sem->m);
    sem->count++;
    pthread_cond_signal(&sem->c);
    pthread_mutex_unlock(&sem->m);
    return 0;
}
int SDL_SemWait(SDL_sem *sem) {
    pthread_mutex_lock(&sem->m);
    while (!sem->count) pthread_cond_wait(&sem->c, &sem->m);
    sem->count--;
    pthread_mutex_unlock(&sem->m);
    return 0;
}

/* Pixel formats and palettes */

static Uint32 format_enum(int bits, Uint32 r, Uint32 g, Uint32 b, Uint32 a) {
    static const struct { int bits; Uint32 r, g, b, a, format; } known[] = {
        {8, 0, 0, 0, 0, SDL_PIXELFORMAT_INDEX8},
        {15, 0x7C00, 0x03E0, 0x001F, 0, SDL_PIXELFORMAT_RGB555},
        {16, 0x7C00, 0x03E0, 0x001F, 0, SDL_PIXELFORMAT_RGB555},
        {16, 0xF800, 0x07E0, 0x001F, 0, SDL_PIXELFORMAT_RGB565},
        {24, 0xFF0000, 0x00FF00, 0x0000FF, 0, SDL_PIXELFORMAT_RGB24},
        {24, 0x0000FF, 0x00FF00, 0xFF0000, 0, SDL_PIXELFORMAT_BGR24},
        {32, 0xFF0000, 0x00FF00, 0x0000FF, 0, SDL_PIXELFORMAT_XRGB8888},
        {32, 0x0000FF, 0x00FF00, 0xFF0000, 0, SDL_PIXELFORMAT_XBGR8888},
        {32, 0xFF0000, 0x00FF00, 0x0000FF, 0xFF000000, SDL_PIXELFORMAT_ARGB8888},
        {32, 0x0000FF, 0x00FF00, 0xFF0000, 0xFF000000, SDL_PIXELFORMAT_ABGR8888},
    };
    for (const auto &k : known)
        if (k.bits == bits && k.r == r && k.g == g && k.b == b && k.a == a) return k.format;
    return SDL_PIXELFORMAT_UNKNOWN;
}

const char *SDL_GetPixelFormatName(Uint32 format) {
    switch (format) {
        case SDL_PIXELFORMAT_INDEX8: return "SDL_PIXELFORMAT_INDEX8";
        case SDL_PIXELFORMAT_RGB555: return "SDL_PIXELFORMAT_RGB555";
        case SDL_PIXELFORMAT_RGB565: return "SDL_PIXELFORMAT_RGB565";
        case SDL_PIXELFORMAT_RGB24: return "SDL_PIXELFORMAT_RGB24";
        case SDL_PIXELFORMAT_BGR24: return "SDL_PIXELFORMAT_BGR24";
        case SDL_PIXELFORMAT_XRGB8888: return "SDL_PIXELFORMAT_RGB888";
        case SDL_PIXELFORMAT_XBGR8888: return "SDL_PIXELFORMAT_BGR888";
        case SDL_PIXELFORMAT_ARGB8888: return "SDL_PIXELFORMAT_ARGB8888";
        case SDL_PIXELFORMAT_ABGR8888: return "SDL_PIXELFORMAT_ABGR8888";
        default: return "SDL_PIXELFORMAT_UNKNOWN";
    }
}

static void channel(Uint32 mask, Uint8 &shift, Uint8 &loss) {
    shift = 0;
    loss = 8;
    if (!mask) return;
    while (!(mask & 1)) { mask >>= 1; shift++; }
    while (mask & 1) { mask >>= 1; loss--; }
}

static SDL_PixelFormat *alloc_format(int bits, Uint32 r, Uint32 g, Uint32 b, Uint32 a) {
    SDL_PixelFormat *f = (SDL_PixelFormat *)calloc(1, sizeof(SDL_PixelFormat));
    f->format = format_enum(bits, r, g, b, a);
    f->BitsPerPixel = (Uint8)bits;
    f->BytesPerPixel = (Uint8)((bits + 7) / 8);
    f->Rmask = r; f->Gmask = g; f->Bmask = b; f->Amask = a;
    channel(r, f->Rshift, f->Rloss);
    channel(g, f->Gshift, f->Gloss);
    channel(b, f->Bshift, f->Bloss);
    channel(a, f->Ashift, f->Aloss);
    f->refcount = 1;
    return f;
}

SDL_Palette *SDL_AllocPalette(int ncolors) {
    SDL_Palette *p = (SDL_Palette *)calloc(1, sizeof(SDL_Palette));
    p->ncolors = ncolors;
    p->colors = (SDL_Color *)calloc((size_t)ncolors, sizeof(SDL_Color));
    for (int i = 0; i < ncolors; i++) p->colors[i] = SDL_Color{255, 255, 255, 255};
    p->refcount = 1;
    return p;
}

void SDL_FreePalette(SDL_Palette *p) {
    if (!p || --p->refcount > 0) return;
    free(p->colors);
    free(p);
}

int SDL_SetPaletteColors(SDL_Palette *p, const SDL_Color *colors, int first, int n) {
    if (!p) return SDL_SetError("no palette");
    if (first + n > p->ncolors) n = p->ncolors - first;
    if (n > 0) memcpy(p->colors + first, colors, (size_t)n * sizeof(SDL_Color));
    p->version++;
    return 0;
}

void SDL_FreeFormat(SDL_PixelFormat *f) {
    if (!f || --f->refcount > 0) return;
    SDL_FreePalette(f->palette);
    free(f);
}

static Uint32 pack(const SDL_PixelFormat *f, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    return ((Uint32)(r >> f->Rloss) << f->Rshift & f->Rmask) | ((Uint32)(g >> f->Gloss) << f->Gshift & f->Gmask) |
           ((Uint32)(b >> f->Bloss) << f->Bshift & f->Bmask) | ((Uint32)(a >> f->Aloss) << f->Ashift & f->Amask);
}

Uint32 SDL_MapRGBA(const SDL_PixelFormat *f, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    if (!f->palette) return pack(f, r, g, b, a);
    Uint32 best = 0, best_d = ~0u;  /* nearest palette entry */
    for (int i = 0; i < f->palette->ncolors; i++) {
        const SDL_Color &c = f->palette->colors[i];
        int dr = c.r - r, dg = c.g - g, db = c.b - b, da = c.a - a;
        Uint32 d = (Uint32)(dr * dr + dg * dg + db * db + da * da);
        if (d < best_d) { best = (Uint32)i; best_d = d; }
    }
    return best;
}

Uint32 SDL_MapRGB(const SDL_PixelFormat *f, Uint8 r, Uint8 g, Uint8 b) { return SDL_MapRGBA(f, r, g, b, 255); }

static Uint8 expand(Uint32 pixel, Uint32 mask, Uint8 shift, Uint8 loss) {
    Uint32 v = (pixel & mask) >> shift << loss;
    return (Uint8)(v | (loss ? v >> (8 - loss) : 0));  /* replicate high bits into the low */
}

void SDL_GetRGBA(Uint32 pixel, const SDL_PixelFormat *f, Uint8 *r, Uint8 *g, Uint8 *b, Uint8 *a) {
    if (f->palette) {
        const SDL_Color c = pixel < (Uint32)f->palette->ncolors ? f->palette->colors[pixel] : SDL_Color{0, 0, 0, 255};
        *r = c.r; *g = c.g; *b = c.b; *a = c.a;
        return;
    }
    *r = expand(pixel, f->Rmask, f->Rshift, f->Rloss);
    *g = expand(pixel, f->Gmask, f->Gshift, f->Gloss);
    *b = expand(pixel, f->Bmask, f->Bshift, f->Bloss);
    *a = f->Amask ? expand(pixel, f->Amask, f->Ashift, f->Aloss) : 255;
}

/* Surfaces */

SDL_Surface *SDL_CreateRGBSurfaceFrom(void *pixels, int w, int h, int depth, int pitch,
                                      Uint32 r, Uint32 g, Uint32 b, Uint32 a) {
    SDL_Surface *s = (SDL_Surface *)calloc(1, sizeof(SDL_Surface));
    s->format = alloc_format(depth, r, g, b, a);
    if (depth <= 8) s->format->palette = SDL_AllocPalette(1 << depth);
    s->w = w;
    s->h = h;
    s->pitch = pitch;
    s->pixels = pixels;
    s->flags = SDL_PREALLOC;
    s->clip_rect = SDL_Rect{0, 0, w, h};
    s->refcount = 1;
    return s;
}

SDL_Surface *SDL_CreateRGBSurface(Uint32, int w, int h, int depth, Uint32 r, Uint32 g, Uint32 b, Uint32 a) {
    const int pitch = (w * ((depth + 7) / 8) + 3) & ~3;
    void *pixels = calloc(1, (size_t)pitch * (size_t)(h ? h : 1));
    if (!pixels) { SDL_Error(SDL_ENOMEM); return NULL; }
    SDL_Surface *s = SDL_CreateRGBSurfaceFrom(pixels, w, h, depth, pitch, r, g, b, a);
    s->flags = 0;
    return s;
}

void SDL_FreeSurface(SDL_Surface *s) {
    if (!s || --s->refcount > 0) return;
    if (!(s->flags & SDL_PREALLOC)) free(s->pixels);
    SDL_FreeFormat(s->format);
    free(s);
}

int SDL_LockSurface(SDL_Surface *s) { s->locked++; return 0; }
void SDL_UnlockSurface(SDL_Surface *s) { if (s->locked) s->locked--; }

int SDL_SetSurfacePalette(SDL_Surface *s, SDL_Palette *p) {
    if (!s->format->palette) return SDL_SetError("surface has no palette");
    if (p) p->refcount++;
    SDL_FreePalette(s->format->palette);
    s->format->palette = p;
    return 0;
}

/* Blits copy and convert; alpha and blend modes are recorded nowhere, as
 * nothing reads the result of blending on a surface nobody sees. */
int SDL_SetSurfaceAlphaMod(SDL_Surface *, Uint8) { return 0; }
int SDL_SetSurfaceBlendMode(SDL_Surface *, SDL_BlendMode) { return 0; }

static Uint32 read_pixel(const Uint8 *p, int bytes) {
    switch (bytes) {
        case 1: return *p;
        case 2: return *(const Uint16 *)p;
        case 3: return (Uint32)p[0] | (Uint32)p[1] << 8 | (Uint32)p[2] << 16;
        default: return *(const Uint32 *)p;
    }
}

static void write_pixel(Uint8 *p, int bytes, Uint32 v) {
    switch (bytes) {
        case 1: *p = (Uint8)v; break;
        case 2: *(Uint16 *)p = (Uint16)v; break;
        case 3: p[0] = (Uint8)v; p[1] = (Uint8)(v >> 8); p[2] = (Uint8)(v >> 16); break;
        default: *(Uint32 *)p = v; break;
    }
}

static bool same_format(const SDL_PixelFormat *a, const SDL_PixelFormat *b) {
    return a->BitsPerPixel == b->BitsPerPixel && a->Rmask == b->Rmask && a->Gmask == b->Gmask &&
           a->Bmask == b->Bmask && a->Amask == b->Amask && !a->palette == !b->palette;
}

static bool intersect(const SDL_Rect &a, const SDL_Rect &b, SDL_Rect &out) {
    const int x0 = SDL_max(a.x, b.x), y0 = SDL_max(a.y, b.y);
    const int x1 = SDL_min(a.x + a.w, b.x + b.w), y1 = SDL_min(a.y + a.h, b.y + b.h);
    out = SDL_Rect{x0, y0, SDL_max(0, x1 - x0), SDL_max(0, y1 - y0)};
    return out.w > 0 && out.h > 0;
}

/* Copies the w x h area at src (sx,sy) scaled onto dst's area d, which lies
 * within dst's clip rect; sx0/sy0 are where d's origin falls in the source. */
static void copy_area(SDL_Surface *src, const SDL_Rect &from, SDL_Surface *dst, const SDL_Rect &to, const SDL_Rect &d) {
    const int sb = src->format->BytesPerPixel, db = dst->format->BytesPerPixel;
    const bool scaled = from.w != to.w || from.h != to.h;
    const bool same = same_format(src->format, dst->format) && (!src->format->palette || src->format->palette == dst->format->palette);
    for (int y = d.y; y < d.y + d.h; y++) {
        const int syy = from.y + (int)((Sint64)(y - to.y) * from.h / to.h);
        const Uint8 *srow = (const Uint8 *)src->pixels + (size_t)syy * (size_t)src->pitch;
        Uint8 *drow = (Uint8 *)dst->pixels + (size_t)y * (size_t)dst->pitch;
        if (same && !scaled) {
            memmove(drow + (size_t)d.x * db, srow + (size_t)(from.x + d.x - to.x) * sb, (size_t)d.w * db);
            continue;
        }
        for (int x = d.x; x < d.x + d.w; x++) {
            const int sxx = from.x + (int)((Sint64)(x - to.x) * from.w / to.w);
            Uint32 v = read_pixel(srow + (size_t)sxx * sb, sb);
            if (!same) {
                Uint8 r, g, b, a;
                SDL_GetRGBA(v, src->format, &r, &g, &b, &a);
                v = SDL_MapRGBA(dst->format, r, g, b, a);
            }
            write_pixel(drow + (size_t)x * db, db, v);
        }
    }
}

int SDL_UpperBlitScaled(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect) {
    if (!src || !dst) return SDL_SetError("blit to or from no surface");
    SDL_Rect from = srcrect ? *srcrect : SDL_Rect{0, 0, src->w, src->h};
    SDL_Rect to = dstrect ? *dstrect : SDL_Rect{0, 0, dst->w, dst->h};
    SDL_Rect d;
    if (!intersect(from, SDL_Rect{0, 0, src->w, src->h}, from) || to.w <= 0 || to.h <= 0 ||
        !intersect(to, dst->clip_rect, d)) {
        if (dstrect) dstrect->w = dstrect->h = 0;
        return 0;
    }
    copy_area(src, from, dst, to, d);
    if (dstrect) *dstrect = d;
    return 0;
}

int SDL_UpperBlit(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect) {
    if (!src || !dst) return SDL_SetError("blit to or from no surface");
    SDL_Rect from = srcrect ? *srcrect : SDL_Rect{0, 0, src->w, src->h};
    SDL_Rect to{dstrect ? dstrect->x : 0, dstrect ? dstrect->y : 0, 0, 0};
    SDL_Rect clipped;  /* clip the source to its surface, moving the destination with it */
    intersect(from, SDL_Rect{0, 0, src->w, src->h}, clipped);
    to.x += clipped.x - from.x;
    to.y += clipped.y - from.y;
    to.w = clipped.w;
    to.h = clipped.h;
    SDL_Rect d;
    if (!intersect(to, dst->clip_rect, d)) {
        if (dstrect) dstrect->w = dstrect->h = 0;
        return 0;
    }
    copy_area(src, clipped, dst, to, d);
    if (dstrect) *dstrect = d;
    return 0;
}

int SDL_FillRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color) {
    if (!dst) return SDL_SetError("fill of no surface");
    SDL_Rect d;
    if (!intersect(rect ? *rect : SDL_Rect{0, 0, dst->w, dst->h}, dst->clip_rect, d)) return 0;
    const int bytes = dst->format->BytesPerPixel;
    for (int y = d.y; y < d.y + d.h; y++) {
        Uint8 *row = (Uint8 *)dst->pixels + (size_t)y * (size_t)dst->pitch + (size_t)d.x * bytes;
        for (int x = 0; x < d.w; x++) write_pixel(row + (size_t)x * bytes, bytes, color);
    }
    return 0;
}

/* Displays and windows: one display, and windows that are surfaces */

static const int display_w = 1920, display_h = 1080;

static void display_mode(SDL_DisplayMode *mode, int w, int h) {
    *mode = SDL_DisplayMode{SDL_PIXELFORMAT_XRGB8888, w, h, 60, NULL};
}

int SDL_GetNumVideoDisplays(void) { return 1; }
int SDL_GetDesktopDisplayMode(int, SDL_DisplayMode *mode) { display_mode(mode, display_w, display_h); return 0; }
int SDL_GetCurrentDisplayMode(int, SDL_DisplayMode *mode) { display_mode(mode, display_w, display_h); return 0; }
int SDL_GetDisplayBounds(int, SDL_Rect *rect) { *rect = SDL_Rect{0, 0, display_w, display_h}; return 0; }

struct SDL_Window {
    int x, y, w, h;
    Uint32 flags;
    SDL_bool grab;
    SDL_Surface *surface;
};

SDL_Window *SDL_CreateWindow(const char *, int x, int y, int w, int h, Uint32 flags) {
    return new SDL_Window{SDL_WINDOWPOS_ISUNDEFINED(x) || SDL_WINDOWPOS_ISCENTERED(x) ? 0 : x,
                          SDL_WINDOWPOS_ISUNDEFINED(y) || SDL_WINDOWPOS_ISCENTERED(y) ? 0 : y,
                          w, h, (flags & ~(Uint32)SDL_WINDOW_OPENGL) | SDL_WINDOW_SHOWN, SDL_FALSE, NULL};
}

void SDL_DestroyWindow(SDL_Window *window) {
    if (!window) return;
    SDL_FreeSurface(window->surface);
    delete window;
}

SDL_Surface *SDL_GetWindowSurface(SDL_Window *window) {
    if (window->surface && (window->surface->w != window->w || window->surface->h != window->h)) {
        SDL_FreeSurface(window->surface);
        window->surface = NULL;
    }
    if (!window->surface)
        window->surface = SDL_CreateRGBSurface(0, window->w, window->h, 32, 0xFF0000, 0x00FF00, 0x0000FF, 0);
    return window->surface;
}

int SDL_UpdateWindowSurface(SDL_Window *) { return 0; }
int SDL_UpdateWindowSurfaceRects(SDL_Window *, const SDL_Rect *, int) { return 0; }

void SDL_GetWindowSize(SDL_Window *window, int *w, int *h) {
    if (w) *w = window->w;
    if (h) *h = window->h;
}
void SDL_SetWindowSize(SDL_Window *window, int w, int h) { window->w = w; window->h = h; }
void SDL_SetWindowIcon(SDL_Window *, SDL_Surface *) {}
void SDL_GetWindowPosition(SDL_Window *window, int *x, int *y) {
    if (x) *x = window->x;
    if (y) *y = window->y;
}
void SDL_SetWindowPosition(SDL_Window *window, int x, int y) {
    if (!SDL_WINDOWPOS_ISUNDEFINED(x) && !SDL_WINDOWPOS_ISCENTERED(x)) window->x = x;
    if (!SDL_WINDOWPOS_ISUNDEFINED(y) && !SDL_WINDOWPOS_ISCENTERED(y)) window->y = y;
}
Uint32 SDL_GetWindowFlags(SDL_Window *window) { return window->flags; }
Uint32 SDL_GetWindowPixelFormat(SDL_Window *) { return SDL_PIXELFORMAT_XRGB8888; }
int SDL_GetWindowDisplayMode(SDL_Window *window, SDL_DisplayMode *mode) { display_mode(mode, window->w, window->h); return 0; }
int SDL_SetWindowDisplayMode(SDL_Window *, const SDL_DisplayMode *) { return 0; }
int SDL_SetWindowFullscreen(SDL_Window *window, Uint32 flags) {
    window->flags = (window->flags & ~(Uint32)SDL_WINDOW_FULLSCREEN_DESKTOP) | flags;
    return 0;
}
void SDL_SetWindowResizable(SDL_Window *window, SDL_bool on) {
    window->flags = on ? window->flags | SDL_WINDOW_RESIZABLE : window->flags & ~(Uint32)SDL_WINDOW_RESIZABLE;
}
void SDL_SetWindowKeyboardGrab(SDL_Window *window, SDL_bool grab) { window->grab = grab; }
SDL_bool SDL_GetWindowKeyboardGrab(SDL_Window *window) { return window->grab; }
void SDL_SetWindowMinimumSize(SDL_Window *, int, int) {}
int SDL_SetWindowOpacity(SDL_Window *, float) { return 0; }
void SDL_SetWindowTitle(SDL_Window *, const char *) {}
void SDL_MaximizeWindow(SDL_Window *) {}
SDL_bool SDL_GetWindowWMInfo(SDL_Window *, SDL_SysWMinfo *) { return SDL_FALSE; }  /* no host window */

/* Only the GPU outputs create these, and the headless build has none. */
void SDL_DestroyRenderer(SDL_Renderer *) {}
void SDL_DestroyTexture(SDL_Texture *) {}

/* Input: no devices, so no events but the ones the emulator pushes */

static pthread_mutex_t events_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t events_pushed = PTHREAD_COND_INITIALIZER;
static std::deque<SDL_Event> events;

int SDL_PushEvent(SDL_Event *event) {
    pthread_mutex_lock(&events_lock);
    events.push_back(*event);
    pthread_cond_signal(&events_pushed);
    pthread_mutex_unlock(&events_lock);
    return 1;
}

int SDL_PollEvent(SDL_Event *event) {
    pthread_mutex_lock(&events_lock);
    const bool any = !events.empty();
    if (any) {
        if (event) {
            *event = events.front();
            events.pop_front();
        }
    }
    pthread_mutex_unlock(&events_lock);
    return any;
}

int SDL_WaitEvent(SDL_Event *event) {
    pthread_mutex_lock(&events_lock);
    while (events.empty()) pthread_cond_wait(&events_pushed, &events_lock);
    if (event) {
        *event = events.front();
        events.pop_front();
    }
    pthread_mutex_unlock(&events_lock);
    return 1;
}

SDL_Keymod SDL_GetModState(void) { return KMOD_NONE; }
SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode) { return SDL_SCANCODE_UNKNOWN; }
const char *SDL_GetScancodeName(SDL_Scancode) { return ""; }
int SDL_ShowCursor(int) { return 0; }
int SDL_SetRelativeMouseMode(SDL_bool) { return 0; }
void SDL_StartTextInput(void) {}
void SDL_StopTextInput(void) {}
void SDL_SetTextInputRect(const SDL_Rect *) {}

int SDL_NumJoysticks(void) { return 0; }
const char *SDL_JoystickNameForIndex(int) { return NULL; }
SDL_Joystick *SDL_JoystickOpen(int) { SDL_SetError("headless: no joysticks"); return NULL; }
void SDL_JoystickClose(SDL_Joystick *) {}
void SDL_JoystickUpdate(void) {}
int SDL_JoystickEventState(int) { return SDL_DISABLE; }
int SDL_JoystickNumAxes(SDL_Joystick *) { return 0; }
int SDL_JoystickNumButtons(SDL_Joystick *) { return 0; }
int SDL_JoystickNumHats(SDL_Joystick *) { return 0; }
Sint16 SDL_JoystickGetAxis(SDL_Joystick *, int) { return 0; }
Uint8 SDL_JoystickGetButton(SDL_Joystick *, int) { return 0; }
Uint8 SDL_JoystickGetHat(SDL_Joystick *, int) { return SDL_HAT_CENTERED; }

/* Audio: no device opens */

SDL_AudioDeviceID SDL_OpenAudioDevice(const char *, int, const SDL_AudioSpec *, SDL_AudioSpec *, int) {
    SDL_SetError("headless: no audio");
    return 0;
}
void SDL_CloseAudioDevice(SDL_AudioDeviceID) {}
void SDL_PauseAudioDevice(SDL_AudioDeviceID, int) {}
void SDL_LockAudioDevice(SDL_AudioDeviceID) {}
void SDL_UnlockAudioDevice(SDL_AudioDeviceID) {}

/* Clipboard: the process's own, not the host's */

static std::string clipboard;

int SDL_SetClipboardText(const char *text) { clipboard = text ? text : ""; return 0; }
char *SDL_GetClipboardText(void) { return strdup(clipboard.c_str()); }
SDL_bool SDL_HasClipboardText(void) { return clipboard.empty() ? SDL_FALSE : SDL_TRUE; }

/* RWops over stdio files and memory */

static Sint64 file_size(SDL_RWops *rw) {
    FILE *fp = (FILE *)rw->hidden.unknown.data1;
    const off_t here = ftello(fp);
    fseeko(fp, 0, SEEK_END);
    const off_t size = ftello(fp);
    fseeko(fp, here, SEEK_SET);
    return size;
}
static Sint64 file_seek(SDL_RWops *rw, Sint64 offset, int whence) {
    FILE *fp = (FILE *)rw->hidden.unknown.data1;
    return fseeko(fp, (off_t)offset, whence) == 0 ? (Sint64)ftello(fp) : SDL_SetError("seek failed");
}
static size_t file_read(SDL_RWops *rw, void *ptr, size_t size, size_t n) { return fread(ptr, size, n, (FILE *)rw->hidden.unknown.data1); }
static size_t file_write(SDL_RWops *rw, const void *ptr, size_t size, size_t n) { return fwrite(ptr, size, n, (FILE *)rw->hidden.unknown.data1); }
static int file_close(SDL_RWops *rw) {
    const int status = fclose((FILE *)rw->hidden.unknown.data1);
    free(rw);
    return status;
}

SDL_RWops *SDL_RWFromFile(const char *path, const char *mode) {
    FILE *fp = fopen(path, mode);
    if (!fp) { SDL_SetError("Couldn't open %s", path); return NULL; }
    SDL_RWops *rw = (SDL_RWops *)calloc(1, sizeof(SDL_RWops));
    rw->size = file_size;
    rw->seek = file_seek;
    rw->read = file_read;
    rw->write = file_write;
    rw->close = file_close;
    rw->type = SDL_RWOPS_STDFILE;
    rw->hidden.unknown.data1 = fp;
    return rw;
}

static Sint64 mem_size(SDL_RWops *rw) { return rw->hidden.mem.stop - rw->hidden.mem.base; }
static Sint64 mem_seek(SDL_RWops *rw, Sint64 offset, int whence) {
    Uint8 *base = whence == RW_SEEK_SET ? rw->hidden.mem.base : whence == RW_SEEK_CUR ? rw->hidden.mem.here : rw->hidden.mem.stop;
    Uint8 *to = base + offset;
    if (to < rw->hidden.mem.base) to = rw->hidden.mem.base;
    if (to > rw->hidden.mem.stop) to = rw->hidden.mem.stop;
    rw->hidden.mem.here = to;
    return to - rw->hidden.mem.base;
}
static size_t mem_read(SDL_RWops *rw, void *ptr, size_t size, size_t n) {
    if (!size) return 0;
    const size_t avail = (size_t)(rw->hidden.mem.stop - rw->hidden.mem.here) / size;
    if (n > avail) n = avail;
    memcpy(ptr, rw->hidden.mem.here, n * size);
    rw->hidden.mem.here += n * size;
    return n;
}
static size_t mem_write(SDL_RWops *rw, const void *ptr, size_t size, size_t n) {
    if (!size) return 0;
    const size_t room = (size_t)(rw->hidden.mem.stop - rw->hidden.mem.here) / size;
    if (n > room) n = room;
    memcpy(rw->hidden.mem.here, ptr, n * size);
    rw->hidden.mem.here += n * size;
    return n;
}
static int mem_close(SDL_RWops *rw) { free(rw); return 0; }

SDL_RWops *SDL_RWFromMem(void *mem, int size) {
    SDL_RWops *rw = (SDL_RWops *)calloc(1, sizeof(SDL_RWops));
    rw->size = mem_size;
    rw->seek = mem_seek;
    rw->read = mem_read;
    rw->write = mem_write;
    rw->close = mem_close;
    rw->type = SDL_RWOPS_MEMORY;
    rw->hidden.mem.base = rw->hidden.mem.here = (Uint8 *)mem;
    rw->hidden.mem.stop = (Uint8 *)mem + size;
    return rw;
}

Sint64 SDL_RWseek(SDL_RWops *rw, Sint64 offset, int whence) { return rw->seek(rw, offset, whence); }
Sint64 SDL_RWtell(SDL_RWops *rw) { return rw->seek(rw, 0, RW_SEEK_CUR); }
size_t SDL_RWread(SDL_RWops *rw, void *ptr, size_t size, size_t n) { return rw->read(rw, ptr, size, n); }
int SDL_RWclose(SDL_RWops *rw) { return rw->close(rw); }

#endif /* C_HEADLESS */
