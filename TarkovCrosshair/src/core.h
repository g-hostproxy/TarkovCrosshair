/*
 * TarkovCrosshair - portable core
 *   - anti-aliased crosshair rasterizer (signed distance fields)
 *   - TarkovSway: spring/damper weapon-inertia model + walk bob
 * No OS dependencies: this file is unit-tested on Linux.
 */
#ifndef TC_CORE_H
#define TC_CORE_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifndef TC_PI
#define TC_PI 3.14159265358979f
/* Movement speed 0..~1.5, used by dynamic spread. */
#endif

/* ------------------------------------------------------------------ */
/* Crosshair                                                           */
/* ------------------------------------------------------------------ */

enum { SH_CROSS, SH_T, SH_X, SH_DOT, SH_CIRCLE, SH_RETICLE, SH_CHEVRON, SH_BOX, SH_COUNT };

static const char *const TC_SHAPE_NAMES[SH_COUNT] = {
    "CROSS", "T-SHAPE", "X", "DOT", "CIRCLE", "RETICLE", "CHEVRON", "BOX"
};

typedef struct {
    int      shape;
    float    size;       /* arm length / radius, px            */
    float    thick;      /* line thickness, px                 */
    float    gap;        /* distance from centre to arms, px   */
    float    opacity;    /* 0..1                               */
    int      dot;        /* centre dot on/off                  */
    float    dotSize;    /* dot diameter, px                   */
    int      outline;    /* black outline on/off               */
    float    outlineW;   /* outline width, px                  */
    uint32_t color;      /* 0xRRGGBB                           */
} CrossParams;

enum { PR_BOX, PR_SEG, PR_RING, PR_DISC };
typedef struct { int type; float a, b, c, d, e; } Prim;

static inline float tc_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float tc_maxf(float a, float b) { return a > b ? a : b; }
static inline float tc_minf(float a, float b) { return a < b ? a : b; }

/* Build the primitive list for a crosshair. Returns count (<= 16). */
static int tc_build_prims(const CrossParams *p, Prim *o)
{
    int n = 0;
    float S = p->size, hw = p->thick * 0.5f, G = p->gap;
    switch (p->shape) {
    case SH_CROSS:
    case SH_T:
        /* boxes: (cx, cy, half-x, half-y) */
        o[n++] = (Prim){PR_BOX,  G + S * 0.5f, 0, S * 0.5f, hw, 0};   /* right  */
        o[n++] = (Prim){PR_BOX, -G - S * 0.5f, 0, S * 0.5f, hw, 0};   /* left   */
        o[n++] = (Prim){PR_BOX, 0,  G + S * 0.5f, hw, S * 0.5f, 0};   /* bottom */
        if (p->shape == SH_CROSS)
            o[n++] = (Prim){PR_BOX, 0, -G - S * 0.5f, hw, S * 0.5f, 0}; /* top */
        break;
    case SH_X: {
        float k = 0.70710678f;
        for (int i = 0; i < 4; i++) {
            float sx = (i & 1) ? 1.f : -1.f, sy = (i & 2) ? 1.f : -1.f;
            o[n++] = (Prim){PR_SEG, sx * G * k, sy * G * k, sx * (G + S) * k, sy * (G + S) * k, hw};
        }
        break; }
    case SH_DOT:
        o[n++] = (Prim){PR_DISC, tc_maxf(0.75f, S * 0.25f + hw * 0.5f), 0, 0, 0, 0};
        break;
    case SH_CIRCLE:
        o[n++] = (Prim){PR_RING, S + G, hw, 0, 0, 0};
        break;
    case SH_RETICLE: {
        float R = S + G, t = tc_maxf(2.f, S * 0.45f);
        o[n++] = (Prim){PR_RING, R, hw, 0, 0, 0};
        o[n++] = (Prim){PR_BOX,  R + t * 0.25f, 0, t * 0.75f, hw, 0};
        o[n++] = (Prim){PR_BOX, -R - t * 0.25f, 0, t * 0.75f, hw, 0};
        o[n++] = (Prim){PR_BOX, 0,  R + t * 0.25f, hw, t * 0.75f, 0};
        o[n++] = (Prim){PR_BOX, 0, -R - t * 0.25f, hw, t * 0.75f, 0};
        break; }
    case SH_CHEVRON: {
        float L = S * 0.85f;
        o[n++] = (Prim){PR_SEG, 0, G, -L, G + L, hw};
        o[n++] = (Prim){PR_SEG, 0, G,  L, G + L, hw};
        break; }
    case SH_BOX: {
        float R = S * 0.6f + G;
        o[n++] = (Prim){PR_BOX, 0, -R, R + hw, hw, 0};
        o[n++] = (Prim){PR_BOX, 0,  R, R + hw, hw, 0};
        o[n++] = (Prim){PR_BOX, -R, 0, hw, R + hw, 0};
        o[n++] = (Prim){PR_BOX,  R, 0, hw, R + hw, 0};
        break; }
    }
    if (p->dot && p->shape != SH_DOT)
        o[n++] = (Prim){PR_DISC, tc_maxf(0.5f, p->dotSize * 0.5f), 0, 0, 0, 0};
    return n;
}

/* Max distance any drawn pixel can be from the centre. */
static float tc_extent(const CrossParams *p)
{
    float e = p->gap + p->size * 1.6f + p->thick + (p->dot ? p->dotSize : 0.f);
    if (p->outline) e += p->outlineW;
    return e + 3.f;
}

/* Bitmap side length (square) needed for these params. */
static int tc_bitmap_size(const CrossParams *p)
{
    int w = 2 * (int)ceilf(tc_extent(p)) + 4;
    if (w & 1) w++;
    return w < 8 ? 8 : w;
}

static inline float tc_sdf(const Prim *q, float x, float y)
{
    switch (q->type) {
    case PR_BOX: {
        float dx = fabsf(x - q->a) - q->c, dy = fabsf(y - q->b) - q->d;
        float ox = tc_maxf(dx, 0.f), oy = tc_maxf(dy, 0.f);
        return sqrtf(ox * ox + oy * oy) + tc_minf(tc_maxf(dx, dy), 0.f); }
    case PR_SEG: {
        float vx = q->c - q->a, vy = q->d - q->b;
        float L = sqrtf(vx * vx + vy * vy);
        if (L < 1e-4f) L = 1e-4f;
        float ux = vx / L, uy = vy / L;
        float mx = x - (q->a + q->c) * 0.5f, my = y - (q->b + q->d) * 0.5f;
        float lx = fabsf(mx * ux + my * uy) - L * 0.5f;
        float ly = fabsf(-mx * uy + my * ux) - q->e;
        float ox = tc_maxf(lx, 0.f), oy = tc_maxf(ly, 0.f);
        return sqrtf(ox * ox + oy * oy) + tc_minf(tc_maxf(lx, ly), 0.f); }
    case PR_RING:
        return fabsf(sqrtf(x * x + y * y) - q->a) - q->b;
    default: /* PR_DISC */
        return sqrtf(x * x + y * y) - q->a;
    }
}

/*
 * Render into a premultiplied BGRA buffer (w*h, row-major, top-down).
 * (fx, fy) = sub-pixel offset of the centre, in pixels (0..1).
 * The centre sits on a pixel centre when the thickness is odd so that
 * 1px / 3px lines stay perfectly crisp.
 */
static void tc_render(const CrossParams *p, float fx, float fy, uint32_t *px, int w, int h)
{
    Prim pr[16];
    int n = tc_build_prims(p, pr);
    int th = (int)lroundf(p->thick);
    float base = (th & 1) ? 0.5f : 0.f;
    float cx = (float)(w / 2) + base + fx, cy = (float)(h / 2) + base + fy;
    float ow = p->outline ? p->outlineW : 0.f;
    float op = tc_clampf(p->opacity, 0.f, 1.f);
    float r = (float)((p->color >> 16) & 255), g = (float)((p->color >> 8) & 255), b = (float)(p->color & 255);
    float ext = tc_extent(p);

    memset(px, 0, (size_t)w * h * 4);
    int x0 = (int)floorf(cx - ext), x1 = (int)ceilf(cx + ext);
    int y0 = (int)floorf(cy - ext), y1 = (int)ceilf(cy + ext);
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w; if (y1 > h) y1 = h;

    for (int y = y0; y < y1; y++) {
        float py = (float)y + 0.5f - cy;
        for (int x = x0; x < x1; x++) {
            float pxl = (float)x + 0.5f - cx;
            float d = 1e9f;
            for (int i = 0; i < n; i++) {
                float di = tc_sdf(&pr[i], pxl, py);
                if (di < d) d = di;
            }
            float af = tc_clampf(0.5f - d, 0.f, 1.f);
            float ao = ow > 0.f ? tc_clampf(0.5f - (d - ow), 0.f, 1.f) : 0.f;
            float a = af + ao * (1.f - af);
            if (a <= 0.f) continue;
            a *= op;
            float k = af * op;
            uint32_t A = (uint32_t)(a * 255.f + 0.5f);
            uint32_t R = (uint32_t)(r * k + 0.5f), Gc = (uint32_t)(g * k + 0.5f), B = (uint32_t)(b * k + 0.5f);
            px[y * w + x] = (A << 24) | (R << 16) | (Gc << 8) | B;
        }
    }
}

/* ------------------------------------------------------------------ */
/* TarkovSway                                                          */
/* ------------------------------------------------------------------ */
/*
 * Model (per axis):
 *   The PMC's body velocity v follows key input with an inertia-limited
 *   acceleration (heavier kit = slower). The weapon hangs off the body on
 *   an under-damped spring, so it is pushed opposite to the body's
 *   acceleration and overshoots when you change direction:
 *
 *     x'' = w^2 (target - x) - 2*zeta*w*x' - G * a_body
 *     target = -v * strafeOffset  (+ mouse-look lag)
 *
 *   Like Tarkov, the barrel stays pointed AGAINST the strafe for as long
 *   as you hold the key, after a small initial swing:
 *   Hold A -> crosshair eases RIGHT and stays there; hold D -> LEFT.
 *   Release -> eases back to centre. A,D,A,D -> R,L,R,L sway.
 *   Walking forward dips the barrel, walking back lifts it (y axis).
 *   A figure-eight walk bob (faster/larger when sprinting) rides on top.
 */

typedef struct {
    float intensity;  /* 0..2   overall amplitude                  */
    float inertia;    /* 0..1   kit weight: slower, heavier swing  */
    float bob;        /* 0..2   walk bob amount                    */
    float mouse;      /* 0..2   mouse-look lag (0 = off)           */
    float scale;      /* pixels scale, screen_height / 1080        */
    float strafe;     /* held-strafe barrel offset, screen pixels   */
    float wiggle;     /* 0..1   swing reduction for quick A/D taps  */
    float delay;      /* s      weapon response delay after a key   */
    float fwdDrop;    /* px     barrel dips while walking forward   */
    float backRise;   /* px     barrel rises while walking back     */
} SwayParams;

typedef struct {
    int   left, right, fwd, back, sprint;
    int   jump;               /* 1 on the tick jump was pressed */
    float mouseDx, mouseDy; /* raw mouse counts since last step */
} SwayInput;

typedef struct {
    float x, y, vx, vy;          /* weapon offset (px) & velocity  */
    float bodyX, bodyZ;          /* body velocity, -1..1(.55)      */
    float accX, accZ;            /* body acceleration              */
    float phase, bobAmt, sprintAmt;
    float mvx, mvy;              /* smoothed mouse velocity        */
    float outX, outY;            /* final offset incl. bob (px)    */
    int   curDir;                /* -1 / 0 / +1 current strafe     */
    float curT, lastStride, offT;/* stride timing (s)              */
    float wig, wScale;           /* wiggle energy 0..1, X scale    */
    /* input delay line (Tarkov's weapon reacts ~0.2s after the key) */
    float leanY;                 /* smoothed fwd/back lean (px)    */
    float clock, lastActive;
    int   hHead, hCount;
    struct { float t; signed char ix, iz, sp; } hist[256];
} SwayState;

static void tc_sway_reset(SwayState *s) { memset(s, 0, sizeof *s); s->wScale = 1.f; s->lastStride = 10.f; s->offT = 10.f; s->lastActive = -100.f; }

/* record this tick's keys, return the keys from `delay` seconds ago */
static void tc_sway_delayed(SwayState *s, int ix, int iz, int sp, float delay, int *oix, int *oiz, int *osp)
{
    s->hHead = (s->hHead + 1) & 255;
    s->hist[s->hHead].t = s->clock; s->hist[s->hHead].ix = (signed char)ix;
    s->hist[s->hHead].iz = (signed char)iz; s->hist[s->hHead].sp = (signed char)sp;
    if (s->hCount < 256) s->hCount++;
    if (ix || iz) s->lastActive = s->clock;
    *oix = 0; *oiz = 0; *osp = 0;
    if (delay <= 0.f) { *oix = ix; *oiz = iz; *osp = sp; return; }
    float want = s->clock - delay;
    for (int k = 0; k < s->hCount; k++) {
        int i = (s->hHead - k) & 255;
        if (s->hist[i].t <= want) { *oix = s->hist[i].ix; *oiz = s->hist[i].iz; *osp = s->hist[i].sp; return; }
    }
}

static void tc_sway_step(SwayState *s, const SwayParams *p, const SwayInput *in, float dt)
{
    if (dt <= 0.f) return;
    if (dt > 0.1f) dt = 0.1f;

    float inert = tc_clampf(p->inertia, 0.f, 1.f);
    float sc = p->scale * p->intensity;
    /* calibrated against an in-game laser (Tarkov Default = inertia 0.8, intensity 0.8) */
    float accRate = 13.f - 4.5f * inert;             /* 1/s           */
    float w = 2.f * TC_PI * (2.3f - 1.1f * inert);  /* spring rad/s  */
    float zeta = 0.45f + 0.24f * inert;
    float G = 35.f * sc;                            /* accel -> px (sideways) */
    float steady = p->strafe;                       /* held offset px (not scaled: calibrated on-screen) */

    /* mouse velocity (counts/s), smoothed ~50ms */
    float mvxRaw = in->mouseDx / dt, mvyRaw = in->mouseDy / dt;
    float km = 1.f - expf(-dt / 0.05f);
    s->mvx += (mvxRaw - s->mvx) * km;
    s->mvy += (mvyRaw - s->mvy) * km;

    s->clock += dt;
    int ix, iz, spr;
    tc_sway_delayed(s, (in->right ? 1 : 0) - (in->left ? 1 : 0), (in->fwd ? 1 : 0) - (in->back ? 1 : 0),
                    in->sprint ? 1 : 0, tc_clampf(p->delay, 0.f, 0.6f), &ix, &iz, &spr);
    int sprinting = spr && iz > 0;
    float spdZ = sprinting ? 1.55f : 1.f;
    float spdX = 1.f;
    if (ix && iz) { spdX *= 0.7071f; if (!sprinting) spdZ *= 0.7071f; }

    float mTx = -tc_clampf(s->mvx * 0.0035f, -14.f, 14.f) * p->mouse * p->scale;
    float mTy = -tc_clampf(s->mvy * 0.0030f, -10.f, 10.f) * p->mouse * p->scale;

    /* ---- wiggle detection: fast A/D reversals shrink the swing ---- */
    if (ix != s->curDir) {
        if (s->curDir != 0) { s->lastStride = s->curT; s->offT = 0.f; }
        if (ix != 0) {
            /* direct reversal, or re-tap right after letting go */
            int reversal = (s->curDir == -ix) || (s->curDir == 0 && s->offT < 0.25f);
            if (reversal) {
                float quick = tc_clampf(1.f - (s->lastStride - 0.15f) / 0.45f, 0.f, 1.f); /* 0.15s=1 .. 0.6s=0 */
                s->wig = tc_minf(1.f, s->wig + 0.45f * quick);
            }
            s->curT = 0.f;
        }
        s->curDir = ix;
    }
    if (ix) s->curT += dt; else s->offT += dt;
    {   /* calm down after a long stride or a pause */
        float k = (ix && s->curT > 0.6f) || (!ix && s->offT > 0.4f) ? 3.f : 0.6f;
        s->wig -= s->wig * (1.f - expf(-k * dt));
        float target = 1.f - tc_clampf(p->wiggle, 0.f, 1.f) * s->wig;
        s->wScale += (target - s->wScale) * (1.f - expf(-10.f * dt));
    }
    float Gx = G * s->wScale, steadyX = steady * s->wScale;

    /* jump: weapon dips on take-off, then springs back */
    if (in->jump) s->vy += 95.f * sc;

    /* fixed sub-steps for stable integration */
    int steps = (int)ceilf(dt / (1.f / 480.f));
    float h = dt / (float)steps;
    for (int i = 0; i < steps; i++) {
        float tX = ix * spdX, tZ = iz * spdZ;
        /* stopping is a bit quicker than starting, like Tarkov */
        float rX = (fabsf(tX) < fabsf(s->bodyX) ? 1.35f : 1.f) * accRate;
        float rZ = (fabsf(tZ) < fabsf(s->bodyZ) ? 1.35f : 1.f) * accRate * (sprinting ? 0.8f : 1.f);
        float nX = s->bodyX + (tX - s->bodyX) * (1.f - expf(-rX * h));
        float nZ = s->bodyZ + (tZ - s->bodyZ) * (1.f - expf(-rZ * h));
        s->accX = (nX - s->bodyX) / h;
        s->accZ = (nZ - s->bodyZ) / h;
        s->bodyX = nX; s->bodyZ = nZ;

        float tx = -s->bodyX * steadyX + mTx;
        /* forward = barrel dips, backward = barrel rises (laser-calibrated) */
        float bz = tc_clampf(s->bodyZ, -1.f, 1.f);
        float lt = bz > 0.f ? bz * p->fwdDrop : bz * p->backRise;      /* + = down */
        s->leanY += (lt - s->leanY) * (1.f - expf(-h / 0.25f));
        float ty = mTy;                  /* lean is added directly below, not via the spring */
        float ax = w * w * (tx - s->x) - 2.f * zeta * w * s->vx - Gx * s->accX;
        float ay = w * w * (ty - s->y) - 2.f * zeta * w * s->vy;
        s->vx += ax * h; s->vy += ay * h;
        s->x += s->vx * h; s->y += s->vy * h;
    }

    /* figure-eight walk bob */
    float speed = sqrtf(s->bodyX * s->bodyX + s->bodyZ * s->bodyZ);
    float kb = 1.f - expf(-dt * 7.f);
    s->bobAmt += (tc_minf(speed, 1.6f) - s->bobAmt) * kb;
    s->sprintAmt += ((sprinting ? 1.f : 0.f) - s->sprintAmt) * kb;
    float freq = 1.2f + 0.6f * s->sprintAmt;          /* Tarkov step rate (measured 1.2 Hz) */
    s->phase += 2.f * TC_PI * freq * dt;
    if (s->phase > 2.f * TC_PI * 64.f) s->phase -= 2.f * TC_PI * 64.f;
    float bobK = p->bob * p->scale * s->bobAmt * (1.f + 0.9f * s->sprintAmt);
    float bx = sinf(s->phase) * 1.5f * bobK;           /* kept small: real bob can't be synced to footsteps */
    float by = sinf(2.f * s->phase) * 0.75f * bobK;

    float lim = 45.f * p->scale;
    s->x = tc_clampf(s->x, -lim, lim);
    s->y = tc_clampf(s->y, -lim, lim);
    s->outX = s->x + bx;
    s->outY = s->y + s->leanY + by;
}

/* True when nothing is moving and no input - lets the app idle. */
static int tc_sway_idle(const SwayState *s, const SwayInput *in)
{
    if (in->left || in->right || in->fwd || in->back) return 0;
    if (s->clock - s->lastActive < 0.65f) return 0;   /* delayed input still pending */
    return fabsf(s->x) < 0.02f && fabsf(s->y) < 0.02f &&
           fabsf(s->vx) < 0.05f && fabsf(s->vy) < 0.05f &&
           fabsf(s->bodyX) < 0.002f && fabsf(s->bodyZ) < 0.002f &&
           s->bobAmt < 0.01f && fabsf(s->mvx) < 1.f && fabsf(s->mvy) < 1.f && !in->jump &&
           s->wig < 0.01f && fabsf(s->wScale - 1.f) < 0.01f && fabsf(s->leanY) < 0.02f;
}

static float tc_sway_speed(const SwayState *s)
{
    return sqrtf(s->bodyX * s->bodyX + s->bodyZ * s->bodyZ);
}

/* ------------------------------------------------------------------ */
/* Recoil (visual only - the mouse is never moved)                     */
/* ------------------------------------------------------------------ */
/*
 * Tarkov-style: every shot snaps the barrel up and a little sideways.
 * Climb builds while you hold fire but levels off at "max climb"
 * (the gun stabilises), then recovers back to centre when you stop -
 * slowly while still firing, quickly once you let go.
 */

typedef struct {
    float kickV;     /* px of climb per shot              */
    float kickH;     /* px horizontal wander per shot     */
    float maxClimb;  /* px where climb levels off         */
    float recovery;  /* 0..1 how fast it returns          */
} RecoilParams;

typedef struct {
    float climb, wander;        /* targets (px), climb > 0 = up */
    float x, y, vx, vy;         /* displayed offset (y down +)  */
    float heat;                 /* 0..1 recent fire, for spread */
    float sinceShot;            /* seconds since last shot      */
    uint32_t rng;
} RecoilState;

static void tc_recoil_reset(RecoilState *r) { memset(r, 0, sizeof *r); r->rng = 0x9E3779B9u; r->sinceShot = 10.f; }

static float tc_rand(uint32_t *s)   /* -1..1 */
{
    uint32_t x = *s ? *s : 1u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return (float)(x & 0xFFFFFF) / 8388607.5f - 1.f;
}

static void tc_recoil_shot(RecoilState *r, const RecoilParams *p)
{
    float room = p->maxClimb > 0.f ? tc_clampf(1.f - r->climb / p->maxClimb, 0.f, 1.f) : 0.f;
    r->climb += p->kickV * (0.25f + 0.75f * room);
    if (p->maxClimb > 0.f && r->climb > p->maxClimb * 1.15f) r->climb = p->maxClimb * 1.15f;
    r->wander += tc_rand(&r->rng) * p->kickH;
    r->wander = tc_clampf(r->wander, -p->kickH * 3.f, p->kickH * 3.f);
    r->vy -= p->kickV * 30.f;                      /* the snap of the shot */
    r->vx += tc_rand(&r->rng) * p->kickH * 12.f;
    r->heat = tc_minf(1.f, r->heat + 0.2f);
    r->sinceShot = 0.f;
}

static void tc_recoil_step(RecoilState *r, const RecoilParams *p, float dt)
{
    if (dt <= 0.f) return;
    if (dt > 0.1f) dt = 0.1f;
    r->sinceShot += dt;
    float rate = 1.5f + 9.f * tc_clampf(p->recovery, 0.f, 1.f);   /* 1/s */
    if (r->sinceShot < 0.18f) rate *= 0.2f;                       /* still firing */
    float k = 1.f - expf(-rate * dt);
    r->climb -= r->climb * k;
    r->wander -= r->wander * k;
    r->heat -= r->heat * (1.f - expf(-2.5f * dt));

    float w = 2.f * TC_PI * 8.f, z = 0.6f;
    int steps = (int)ceilf(dt / (1.f / 600.f));
    float h = dt / (float)steps;
    for (int i = 0; i < steps; i++) {
        float ay = w * w * (-r->climb - r->y) - 2.f * z * w * r->vy;
        float ax = w * w * (r->wander - r->x) - 2.f * z * w * r->vx;
        r->vy += ay * h; r->vx += ax * h;
        r->y += r->vy * h; r->x += r->vx * h;
    }
}

static int tc_recoil_idle(const RecoilState *r)
{
    return fabsf(r->climb) < 0.02f && fabsf(r->y) < 0.02f && fabsf(r->vy) < 0.05f &&
           fabsf(r->x) < 0.02f && fabsf(r->vx) < 0.05f && r->heat < 0.01f;
}

#endif
