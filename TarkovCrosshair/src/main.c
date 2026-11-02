/*
 * TarkovCrosshair - lightweight crosshair overlay with TarkovSway.
 *
 * Design rules (performance + anti-cheat hygiene):
 *   - Completely independent of any game: never looks at other windows or
 *     processes, never opens, reads, writes or injects into anything.
 *   - No keyboard/mouse hooks. Input is read by polling GetAsyncKeyState
 *     (and optional Raw Input for mouse-look sway, only while active).
 *   - The overlay is a tiny click-through layered window; it is only
 *     redrawn when something actually changes, and fully hidden (removed
 *     from composition) when the crosshair isn't shown.
 *   - Sleeps on a waitable timer: ~125 Hz when idle, monitor rate
 *     only while the sway animation is moving, 10 Hz when switched off.
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "core.h"

#define APP_NAME     L"TarkovCrosshair"
#define APP_VERSION  L"1.1"
#define WM_TRAY      (WM_APP + 1)
#define TRAY_ID      1
#define IDM_OPEN     100
#define IDM_TOGGLE   101
#define IDM_SWAY     102
#define IDM_EXIT     103
#define WM_APP_QUIT  (WM_APP + 2)
#define WM_APP_COLOR (WM_APP + 3)

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/* ================================================================== */
/* Settings                                                            */
/* ================================================================== */

enum { AIM_HOLD, AIM_TOGGLE, AIM_TARKOV };

typedef struct {
    int   enabled;
    CrossParams xh;
    int   offX, offY;
    int   hideAim, aimMode, hideSprint, startTray;
    int   keyToggle;
    int   swayOn;
    float swayInt, swayInertia, swayBob, swayMouse, swayLeanPx, swayWiggle, swayDelayMs, swayFwdPx, swayBackPx;
    int   cfgVersion;
    int   kLeft, kRight, kFwd, kBack, kSprint;
    int   swayPreset;           /* 0 = Tarkov Default, 1..3 = user slots */
    int   kJump;
    int   recoilOn, fireMode;   /* fireMode: 0 semi, 1 auto */
    float fireRpm, kickV, kickH, maxClimb, recovery;
    int   spreadOn;
    float spreadAmt;
    int   hideInv;
} Settings;

static Settings g_set;
static WCHAR    g_iniPath[MAX_PATH];

static void settings_defaults(Settings *s)
{
    memset(s, 0, sizeof *s);
    s->enabled = 1;
    s->xh = (CrossParams){ SH_CROSS, 6, 2, 3, 1.f, 0, 2, 1, 1, 0x3CFF6E };
    s->hideAim = 1; s->aimMode = AIM_TARKOV;
    s->keyToggle = VK_INSERT;
    s->swayOn = 1; s->swayPreset = 0;
    s->swayInt = 0.8f; s->swayLeanPx = 39.f; s->swayInertia = 0.8f; s->swayBob = 1.2f; s->swayMouse = 0.7f; s->swayWiggle = 0.f; s->swayDelayMs = 235.f; s->swayFwdPx = 22.f; s->swayBackPx = 12.f;
    s->hideSprint = 1;
    s->cfgVersion = 3;
    s->kLeft = 'A'; s->kRight = 'D'; s->kFwd = 'W'; s->kBack = 'S'; s->kSprint = VK_SHIFT; s->kJump = VK_SPACE;
    s->recoilOn = 1; s->fireMode = 1; s->fireRpm = 730.f;
    s->kickV = 3.5f; s->kickH = 1.f; s->maxClimb = 30.f; s->recovery = 0.6f;
    s->spreadOn = 1; s->spreadAmt = 1.f;
    s->hideInv = 0;
}

static void ini_put(const WCHAR *k, double v)
{
    WCHAR b[64];
    swprintf(b, 64, L"%g", v);
    WritePrivateProfileStringW(L"TarkovCrosshair", k, b, g_iniPath);
}
static double ini_get(const WCHAR *k, double def)
{
    WCHAR b[64];
    GetPrivateProfileStringW(L"TarkovCrosshair", k, L"", b, 64, g_iniPath);
    if (!b[0]) return def;
    return wcstod(b, NULL);
}

#define SETTINGS_FIELDS(X) \
    X(enabled, i) X(offX, i) X(offY, i) X(hideAim, i) X(aimMode, i) \
    X(hideSprint, i) X(startTray, i) X(keyToggle, i) X(swayOn, i) X(swayInt, f) \
    X(swayInertia, f) X(swayBob, f) X(swayMouse, f) X(swayLeanPx, f) X(swayWiggle, f) X(swayDelayMs, f) X(swayFwdPx, f) X(swayBackPx, f) X(cfgVersion, i) X(kLeft, i) X(kRight, i) X(kFwd, i) \
    X(kBack, i) X(kSprint, i) X(swayPreset, i) X(kJump, i) X(recoilOn, i) X(fireMode, i) X(fireRpm, f) \
    X(kickV, f) X(kickH, f) X(maxClimb, f) X(recovery, f) X(spreadOn, i) X(spreadAmt, f) X(hideInv, i)
#define XH_FIELDS(X) \
    X(shape, i) X(size, f) X(thick, f) X(gap, f) X(opacity, f) X(dot, i) X(dotSize, f) \
    X(outline, i) X(outlineW, f) X(color, u)

static void slots_save(void);
static void settings_save(void)
{
#define PUT(n, t) ini_put(L"" #n, (double)g_set.n);
#define PUTX(n, t) ini_put(L"xh_" #n, (double)g_set.xh.n);
    SETTINGS_FIELDS(PUT)
    XH_FIELDS(PUTX)
#undef PUT
#undef PUTX
    slots_save();
}

static void settings_load(void)
{
    settings_defaults(&g_set);
    if (GetFileAttributesW(g_iniPath) == INVALID_FILE_ATTRIBUTES) return;
#define CAST_i (int)
#define CAST_f (float)
#define CAST_u (uint32_t)
#define GET(n, t) g_set.n = CAST_##t ini_get(L"" #n, (double)g_set.n);
#define GETX(n, t) g_set.xh.n = CAST_##t ini_get(L"xh_" #n, (double)g_set.xh.n);
    SETTINGS_FIELDS(GET)
    XH_FIELDS(GETX)
#undef GET
#undef GETX
    if (g_set.xh.shape < 0 || g_set.xh.shape >= SH_COUNT) g_set.xh.shape = 0;
    if (g_set.aimMode < 0 || g_set.aimMode > 2) g_set.aimMode = AIM_TARKOV;
}

/* ---------- presets (sway + recoil + spread) ---------- */

/* preset field, matching Settings field, type */
#define PRESET_FIELDS(X) \
    X(intensity, swayInt, f) X(lean, swayLeanPx, f) X(inertia, swayInertia, f) X(bob, swayBob, f) \
    X(mouse, swayMouse, f) X(fireMode, fireMode, i) \
    X(rpm, fireRpm, f) X(kickV, kickV, f) X(kickH, kickH, f) X(maxClimb, maxClimb, f) \
    X(recovery, recovery, f) X(spreadOn, spreadOn, i) X(spread, spreadAmt, f) X(wiggle, swayWiggle, f) X(delayMs, swayDelayMs, f) X(fwd, swayFwdPx, f) X(back, swayBackPx, f)

#define PTYPE_f float
#define PTYPE_i int
#define PDECL(n, sf, t) PTYPE_##t n;
typedef struct { PRESET_FIELDS(PDECL) } Preset;

#define PRESET_SLOTS 3
/* Tuned in game against a laser: built-in and LOCKED (never edited) */
static const Preset PRESET_TARKOV = {
    .intensity = 0.8f, .lean = 39.f, .inertia = 0.8f, .bob = 1.2f, .mouse = 0.7f,
    .fireMode = 1, .rpm = 730.f, .kickV = 3.5f, .kickH = 1.f, .maxClimb = 30.f,
    .recovery = 0.6f, .spreadOn = 1, .spread = 1.f, .wiggle = 0.f, .delayMs = 235.f, .fwd = 22.f, .back = 12.f,
};
static Preset g_slot[PRESET_SLOTS];
static int    g_slotUsed[PRESET_SLOTS];

static Preset preset_current(void)
{
    Preset p;
#define PCUR(n, sf, t) p.n = g_set.sf;
    PRESET_FIELDS(PCUR)
#undef PCUR
    return p;
}
static void preset_apply(const Preset *p)
{
#define PAPP(n, sf, t) g_set.sf = p->n;
    PRESET_FIELDS(PAPP)
#undef PAPP
}
static int preset_equal(const Preset *a, const Preset *b)
{
#define PEQ(n, sf, t) if (fabsf((float)a->n - (float)b->n) > 1e-3f) return 0;
    PRESET_FIELDS(PEQ)
#undef PEQ
    return 1;
}
static const Preset *preset_selected(void)
{
    if (g_set.swayPreset == 0) return &PRESET_TARKOV;
    int i = g_set.swayPreset - 1;
    return (i >= 0 && i < PRESET_SLOTS) ? &g_slot[i] : &PRESET_TARKOV;
}

static void slots_save(void)
{
    WCHAR k[48];
    for (int i = 0; i < PRESET_SLOTS; i++) {
        swprintf(k, 48, L"slot%d_used", i + 1); ini_put(k, g_slotUsed[i]);
#define PS(n, sf, t) swprintf(k, 48, L"slot%d_" L"" #n, i + 1); ini_put(k, (double)g_slot[i].n);
        PRESET_FIELDS(PS)
#undef PS
    }
}
static void slots_load(void)
{
    WCHAR k[48];
    for (int i = 0; i < PRESET_SLOTS; i++) {
        g_slot[i] = PRESET_TARKOV;   /* older slots: missing recoil fields get the defaults */
        swprintf(k, 48, L"slot%d_used", i + 1); g_slotUsed[i] = (int)ini_get(k, 0);
#define PG(n, sf, t) swprintf(k, 48, L"slot%d_" L"" #n, i + 1); g_slot[i].n = (PTYPE_##t)ini_get(k, (double)g_slot[i].n);
        PRESET_FIELDS(PG)
#undef PG
    }
    if (g_set.swayPreset < 0 || g_set.swayPreset > PRESET_SLOTS) g_set.swayPreset = 0;
    /* v3: Tarkov Default locked; every slot starts as an exact copy of it */
    if (g_set.cfgVersion < 3) {
        for (int i = 0; i < PRESET_SLOTS; i++) g_slot[i] = PRESET_TARKOV;
        g_set.cfgVersion = 3;
    }
    for (int i = 0; i < PRESET_SLOTS; i++) g_slotUsed[i] = 1;
    /* live settings always come from the selected preset (Default can't drift) */
    preset_apply(preset_selected());
}

/* ================================================================== */
/* Globals                                                             */
/* ================================================================== */

static HINSTANCE g_inst;
static HWND      g_ov;          /* overlay window             */
static HWND      g_ui;          /* settings window            */
static HICON     g_iconBig, g_iconSmall;
static NOTIFYICONDATAW g_nid;

/* overlay bitmap */
static HDC       g_ovDC;
static HBITMAP   g_ovBmp, g_ovOld;
static uint32_t *g_ovBits;
static int       g_ovW;                 /* bitmap side length   */
static uint32_t *g_cache[16];           /* 4x4 sub-pixel variants */
static int       g_curVariant = -1;
static int       g_bmpVersion = 0;

/* overlay presented state (to skip redundant updates) */
static int  g_ovShown;
static int  g_lastX = INT_MIN, g_lastY = INT_MIN, g_lastAlpha = -1, g_lastVer = -1;

/* runtime state */
static SwayState g_sway;
static RecoilState g_recoil;
static double g_nextShot;
static int    g_prevLmb, g_prevJump, g_inventory, g_testFire;
static int    g_bloomStep;          /* dynamic spread, in half pixels */
static float  g_alpha;          /* 0..1 current overlay alpha */
static int    g_aiming;
static int    g_aimPressWasExit;
static double g_aimPressT;
static int    g_prevRmb, g_prevToggle, g_prevTab, g_prevEsc;
static int    g_uiFg, g_active;
static int    g_centerX, g_centerY, g_screenH = 1080;
static double g_nextFgCheck, g_nextTopmost, g_saveAt, g_nextPreview;
static int    g_frameHz = 144;
static double g_qpcFreq;
static float  g_mouseDx, g_mouseDy;
static int    g_rawOn;
static int    g_sprinting;
static int    g_dirtyUI;
static int   *g_capture;                 /* keybind being captured */

static double now_s(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / g_qpcFreq;
}

static int key_down(int vk) { return vk && (GetAsyncKeyState(vk) & 0x8000) != 0; }

/* ================================================================== */
/* Overlay                                                             */
/* ================================================================== */

static HBITMAP make_dib(int w, int h, uint32_t **bits)
{
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)bits, NULL, 0);
}

static void cache_free(void)
{
    for (int i = 0; i < 16; i++) { free(g_cache[i]); g_cache[i] = NULL; }
    g_curVariant = -1;
}

static float spread_max(void)
{
    return g_set.spreadOn ? g_set.spreadAmt * ((float)g_screenH / 1080.f) * 11.5f : 0.f;
}
static void xh_effective(CrossParams *o)
{
    *o = g_set.xh;
    o->gap += (float)g_bloomStep * 0.5f;
}

/* Called whenever crosshair settings change. */
static void crosshair_rebuild(void)
{
    cache_free();
    CrossParams big = g_set.xh;
    big.gap += spread_max();
    int w = tc_bitmap_size(&big);
    if (w != g_ovW || !g_ovBmp) {
        if (g_ovBmp) { SelectObject(g_ovDC, g_ovOld); DeleteObject(g_ovBmp); }
        g_ovBmp = make_dib(w, w, &g_ovBits);
        g_ovOld = (HBITMAP)SelectObject(g_ovDC, g_ovBmp);
        g_ovW = w;
    }
    g_bmpVersion++;
}

static void select_variant(int v)
{
    if (v == g_curVariant) return;
    size_t bytes = (size_t)g_ovW * g_ovW * 4;
    if (!g_cache[v]) {
        g_cache[v] = (uint32_t *)malloc(bytes);
        if (!g_cache[v]) return;
        CrossParams e;
        xh_effective(&e);
        tc_render(&e, (v & 3) * 0.25f, (v >> 2) * 0.25f, g_cache[v], g_ovW, g_ovW);
    }
    GdiFlush();
    memcpy(g_ovBits, g_cache[v], bytes);
    g_curVariant = v;
    g_bmpVersion++;
}

/* split an offset into integer pixels + quarter-pixel variant */
static void split_offset(float o, int *ip, int *q)
{
    float f = floorf(o);
    int qq = (int)lroundf((o - f) * 4.f);
    *ip = (int)f;
    if (qq == 4) { qq = 0; (*ip)++; }
    *q = qq;
}

/* sway + recoil offset, px */
static void total_offset(float *x, float *y)
{
    float sx = 0.f, sy = 0.f;
    if (g_set.swayOn)   { sx += g_sway.outX; sy += g_sway.outY; }
    if (g_set.recoilOn) { sx += g_recoil.x;  sy += g_recoil.y; }
    *x = sx; *y = sy;
}

static void overlay_present(void)
{
    int alpha = (int)lroundf(g_alpha * 255.f);
    if (alpha <= 0) {
        if (g_ovShown) { ShowWindow(g_ov, SW_HIDE); g_ovShown = 0; }
        g_lastAlpha = 0;
        return;
    }
    int ix, iy, qx, qy;
    float sx, sy;
    total_offset(&sx, &sy);
    split_offset(sx, &ix, &qx);
    split_offset(sy, &iy, &qy);
    select_variant(qy * 4 + qx);

    int x = g_centerX + g_set.offX - g_ovW / 2 + ix;
    int y = g_centerY + g_set.offY - g_ovW / 2 + iy;
    if (x == g_lastX && y == g_lastY && alpha == g_lastAlpha && g_lastVer == g_bmpVersion && g_ovShown)
        return;

    POINT dst = { x, y }, src = { 0, 0 };
    SIZE sz = { g_ovW, g_ovW };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)alpha, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_ov, NULL, &dst, &sz, g_ovDC, &src, 0, &bf, ULW_ALPHA);
    if (!g_ovShown) {
        SetWindowPos(g_ov, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        g_ovShown = 1;
    }
    g_lastX = x; g_lastY = y; g_lastAlpha = alpha; g_lastVer = g_bmpVersion;
}

/* ================================================================== */
/* Environment: foreground window, screen centre, refresh rate         */
/* ================================================================== */

/* The crosshair is centred on the primary monitor. Nothing about other
 * windows or processes is inspected - only whether our own settings
 * window is the focused one (so clicks there don't count as aiming). */
static void update_center(void)
{
    POINT o = { 0, 0 };
    HMONITOR mon = MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi; mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, (MONITORINFO *)&mi);
    g_centerX = (mi.rcMonitor.left + mi.rcMonitor.right) / 2;
    g_centerY = (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2;
    g_screenH = mi.rcMonitor.bottom - mi.rcMonitor.top;
    DEVMODEW dm = {0}; dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
        int hz = (int)dm.dmDisplayFrequency;
        g_frameHz = hz < 60 ? 60 : (hz > 165 ? 165 : hz);
    }
}

static void check_foreground(void)
{
    int uf = (g_ui && GetForegroundWindow() == g_ui);
    if (uf != g_uiFg) { g_uiFg = uf; g_dirtyUI = 1; }
}

/* Raw mouse input only while mouse-look sway is on and we're active. */
static void raw_mouse(int want)
{
    if (want == g_rawOn) return;
    RAWINPUTDEVICE rid = { 0x01, 0x02, want ? RIDEV_INPUTSINK : RIDEV_REMOVE, want ? g_ui : NULL };
    if (RegisterRawInputDevices(&rid, 1, sizeof rid)) g_rawOn = want;
    g_mouseDx = g_mouseDy = 0;
}

/* ================================================================== */
/* Tick                                                                */
/* ================================================================== */

static void reset_runtime(void)
{
    tc_sway_reset(&g_sway);
    tc_recoil_reset(&g_recoil);
    g_aiming = 0;
    g_inventory = 0;
    if (g_bloomStep) { g_bloomStep = 0; cache_free(); g_bmpVersion++; }
}

/* Returns the number of milliseconds (fractional) until the next tick. */
static double tick(void)
{
    static double last;
    double t = now_s();
    float dt = last > 0 ? (float)(t - last) : 0.f;
    last = t;

    if (t >= g_nextFgCheck) { check_foreground(); g_nextFgCheck = t + 0.25; }

    /* show/hide hotkey works anywhere */
    int tg = key_down(g_set.keyToggle);
    if (tg && !g_prevToggle && !g_capture) {
        g_set.enabled = !g_set.enabled;
        g_saveAt = t + 0.5; g_dirtyUI = 1;
    }
    g_prevToggle = tg;

    int wasActive = g_active;
    g_active = g_set.enabled;
    if (wasActive != g_active) { g_dirtyUI = 1; if (!g_active) reset_runtime(); }

    raw_mouse(g_active && g_set.swayOn && g_set.swayMouse > 0.f);

    if (!g_active) {
        g_alpha = 0.f;
        overlay_present();
        g_prevRmb = g_prevTab = g_prevEsc = g_prevLmb = g_prevJump = 0;
        return 100.0;
    }

    /* ---- keys ---- */
    int rmb = key_down(VK_RBUTTON), lmb = key_down(VK_LBUTTON);
    int fwd = key_down(g_set.kFwd), back = key_down(g_set.kBack);
    int tab = key_down(VK_TAB), esc = key_down(VK_ESCAPE), jump = key_down(g_set.kJump);
    g_sprinting = key_down(g_set.kSprint) && fwd && !back;
    if (g_uiFg) { rmb = 0; lmb = g_testFire; } /* clicks in our own window don't count */
    int tabEdge = tab && !g_prevTab && !g_uiFg, escEdge = esc && !g_prevEsc;
    g_prevTab = tab; g_prevEsc = esc;

    /* ---- inventory (Tab toggles, Esc closes) ---- */
    if (g_set.hideInv) {
        if (tabEdge) { g_inventory = !g_inventory; g_dirtyUI = 1; }
        if (escEdge && g_inventory) { g_inventory = 0; g_dirtyUI = 1; }
    } else g_inventory = 0;

    /* ---- aiming ---- */
    switch (g_set.aimMode) {
    case AIM_HOLD:
        g_aiming = rmb;
        break;
    case AIM_TOGGLE:
        if (rmb && !g_prevRmb) g_aiming = !g_aiming;
        break;
    case AIM_TARKOV: /* tap = toggle, hold = hold (Tarkov default) */
        if (rmb && !g_prevRmb) {
            if (g_aiming) { g_aiming = 0; g_aimPressWasExit = 1; }
            else { g_aiming = 1; g_aimPressWasExit = 0; g_aimPressT = t; }
        } else if (!rmb && g_prevRmb) {
            if (!g_aimPressWasExit && t - g_aimPressT > 0.22) g_aiming = 0;
        }
        break;
    }
    /* things that drop you out of ADS in game */
    if (g_set.aimMode != AIM_HOLD && (tabEdge || escEdge || g_sprinting)) g_aiming = 0;
    if (g_inventory) g_aiming = 0;
    if (rmb != g_prevRmb) g_dirtyUI = 1;
    g_prevRmb = rmb;

    int animating = 0;

    /* ---- recoil (visual only) ---- */
    if (g_set.recoilOn) {
        RecoilParams rp = { g_set.kickV * g_screenH / 1080.f, g_set.kickH * g_screenH / 1080.f,
                            g_set.maxClimb * g_screenH / 1080.f, g_set.recovery };
        int canFire = lmb && !g_inventory && !g_sprinting;
        double interval = 60.0 / (g_set.fireRpm > 60.f ? g_set.fireRpm : 60.f);
        if (canFire && !g_prevLmb) {
            tc_recoil_shot(&g_recoil, &rp);
            g_nextShot = t + interval;
        } else if (canFire && g_set.fireMode == 1) {
            if (t - g_nextShot > 0.1) g_nextShot = t;          /* we were asleep */
            while (t >= g_nextShot) { tc_recoil_shot(&g_recoil, &rp); g_nextShot += interval; }
        }
        g_prevLmb = canFire;
        tc_recoil_step(&g_recoil, &rp, dt);
        if (!tc_recoil_idle(&g_recoil)) animating = 1;
        if (canFire && g_set.fireMode == 1) animating = 1;
    } else g_prevLmb = lmb;

    /* ---- sway (also feeds dynamic spread) ---- */
    if (g_set.swayOn || g_set.spreadOn) {
        SwayInput in = {0};
        in.left = key_down(g_set.kLeft); in.right = key_down(g_set.kRight);
        in.fwd = fwd; in.back = back; in.sprint = key_down(g_set.kSprint);
        in.jump = jump && !g_prevJump && !g_uiFg && g_set.swayOn;
        in.mouseDx = g_mouseDx; in.mouseDy = g_mouseDy;
        g_mouseDx = g_mouseDy = 0;
        SwayParams sp = { g_set.swayInt, g_set.swayInertia, g_set.swayBob, g_set.swayMouse,
                          (float)g_screenH / 1080.f, g_set.swayLeanPx, g_set.swayWiggle, g_set.swayDelayMs * 0.001f, g_set.swayFwdPx, g_set.swayBackPx };
        tc_sway_step(&g_sway, &sp, &in, dt);
        if (!tc_sway_idle(&g_sway, &in)) animating = 1;
        else { g_sway.outX = g_sway.outY = 0; g_sway.x = g_sway.y = 0; }
    }
    g_prevJump = jump;

    /* ---- dynamic spread: gap opens while moving / firing ---- */
    {
        int step = 0;
        if (g_set.spreadOn) {
            float spd = tc_minf(tc_sway_speed(&g_sway), 1.6f);
            float bloom = g_set.spreadAmt * ((float)g_screenH / 1080.f) * (spd * 4.f + g_recoil.heat * 5.f);
            if (bloom > spread_max()) bloom = spread_max();
            step = (int)(bloom * 2.f + 0.5f);
        }
        if (step != g_bloomStep) { g_bloomStep = step; cache_free(); g_bmpVersion++; animating = 1; }
    }

    /* ---- visibility / fade ---- */
    int want = !((g_set.hideAim && g_aiming) || (g_set.hideSprint && g_sprinting) || (g_set.hideInv && g_inventory));
    int fading = 0;
    if (!want) g_alpha = 0.f;                         /* hide instantly   */
    else if (g_alpha < 1.f) {                         /* fade back ~90 ms */
        g_alpha += dt / 0.09f;
        if (g_alpha >= 1.f) g_alpha = 1.f; else fading = 1;
    }

    overlay_present();

    if (g_ovShown && t >= g_nextTopmost) {           /* stay above borderless games */
        SetWindowPos(g_ov, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        g_nextTopmost = t + 2.0;
    }

    if (g_ui && IsWindowVisible(g_ui) && !IsIconic(g_ui) && (((animating || fading) && g_uiFg) || g_dirtyUI) && t >= g_nextPreview) {
        InvalidateRect(g_ui, NULL, FALSE);
        g_nextPreview = t + 1.0 / 60.0;
        g_dirtyUI = 0;
    }

    if (animating || fading) return 1000.0 / g_frameHz;
    return 8.0; /* 125 Hz input polling while idle in game */
}

/* ================================================================== */
/* UI toolkit (immediate-mode, GDI, custom drawn)                      */
/* ================================================================== */

#define UI_W 860
#define UI_H 636

#define C_BG      RGB(11, 11, 10)
#define C_HEAD    RGB(7, 7, 6)
#define C_PANEL   RGB(19, 19, 17)
#define C_PANEL2  RGB(28, 28, 25)
#define C_BORDER  RGB(52, 51, 45)
#define C_BORDERH RGB(104, 100, 84)
#define C_TEXT    RGB(201, 197, 178)
#define C_DIM     RGB(124, 121, 106)
#define C_ACCENT  RGB(154, 142, 100)
#define C_ACCENTH RGB(214, 202, 152)
#define C_GREEN   RGB(118, 168, 86)
#define C_RED     RGB(178, 62, 50)
#define C_AMBER   RGB(204, 150, 60)

static float   g_scale = 1.f;
static HDC     g_dc;                    /* back buffer during paint */
static HBITMAP g_backBmp, g_backOld;
static int     g_backW, g_backH;
static HFONT   f_title, f_head, f_body, f_small, f_val, f_big;
static int     g_tab;
static WCHAR   g_tip[160];

static struct { int x, y, down, pressed, wheel; } M;
static int  g_activeId, g_hotId;
static int  g_captureId;

/* shape tile icons */
static HBITMAP g_iconBmp[SH_COUNT][2];
static HDC     g_iconDC;
static int     g_iconW;

static int S(float v) { return (int)lroundf(v * g_scale); }

static HFONT mkfont(float px, int weight)
{
    return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Bahnschrift");
}

static void fill(int x, int y, int w, int h, COLORREF c)
{
    RECT r = { S(x), S(y), S(x + w), S(y + h) };
    SetDCBrushColor(g_dc, c);
    FillRect(g_dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}
static void fillpx(int x, int y, int w, int h, COLORREF c)  /* physical pixels */
{
    RECT r = { x, y, x + w, y + h };
    SetDCBrushColor(g_dc, c);
    FillRect(g_dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}
static void frame(int x, int y, int w, int h, COLORREF c)
{
    int X = S(x), Y = S(y), W = S(x + w) - X, H = S(y + h) - Y;
    fillpx(X, Y, W, 1, c); fillpx(X, Y + H - 1, W, 1, c);
    fillpx(X, Y, 1, H, c); fillpx(X + W - 1, Y, 1, H, c);
}
/* tactical corner brackets */
static void brackets(int x, int y, int w, int h, int len, COLORREF c)
{
    int X = S(x), Y = S(y), X2 = S(x + w) - 1, Y2 = S(y + h) - 1, L = S(len), T = S(2) > 1 ? S(2) : 2;
    fillpx(X, Y, L, T, c); fillpx(X, Y, T, L, c);
    fillpx(X2 - L + 1, Y, L, T, c); fillpx(X2 - T + 1, Y, T, L, c);
    fillpx(X, Y2 - T + 1, L, T, c); fillpx(X, Y2 - L + 1, T, L, c);
    fillpx(X2 - L + 1, Y2 - T + 1, L, T, c); fillpx(X2 - T + 1, Y2 - L + 1, T, L, c);
}
static void text(int x, int y, int w, int h, const WCHAR *s, HFONT f, COLORREF c, UINT fmt, int tracking)
{
    RECT r = { S(x), S(y), S(x + w), S(y + h) };
    SelectObject(g_dc, f);
    SetTextColor(g_dc, c);
    SetTextCharacterExtra(g_dc, tracking ? S((float)tracking * 0.5f) : 0);
    DrawTextW(g_dc, s, -1, &r, fmt | DT_SINGLELINE | DT_NOPREFIX);
}
/* small wrapped note: never clips, wraps to as many lines as fit */
static void note(int x, int y, int w, int h, const WCHAR *s, COLORREF c)
{
    RECT r = { S(x), S(y), S(x + w), S(y + h) };
    SelectObject(g_dc, f_small);
    SetTextColor(g_dc, c);
    SetTextCharacterExtra(g_dc, 0);
    DrawTextW(g_dc, s, -1, &r, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
}

static int hit(int x, int y, int w, int h)
{
    return M.x >= S(x) && M.x < S(x + w) && M.y >= S(y) && M.y < S(y + h);
}
static void heading(int x, int y, int w, const WCHAR *s)
{
    text(x, y, w, 16, s, f_head, C_DIM, DT_LEFT | DT_VCENTER, 3);
    SIZE sz; SelectObject(g_dc, f_head);
    SetTextCharacterExtra(g_dc, S(1.5f));
    GetTextExtentPoint32W(g_dc, s, (int)wcslen(s), &sz);
    int lx = S(x) + sz.cx + S(10);
    if (lx < S(x + w)) fillpx(lx, S(y + 8), S(x + w) - lx, 1, C_BORDER);
}
/* g_ro: widgets drawn while it is set are read-only (Tarkov Default is locked) */
static int g_ro;
static void tip(int id, const WCHAR *s) { if (!g_ro && g_hotId == id) wcsncpy(g_tip, s, 159); }
#define C_LOCK RGB(88, 86, 76)
#define LOCK_TIP L"Tarkov Default is locked. Pick SLOT 1-3 (they start as copies) to adjust this."
static int ro_hover(int id, int hv)
{
    if (!g_ro) return 0;
    if (hv) { g_hotId = id; wcsncpy(g_tip, LOCK_TIP, 159); }
    return 1;
}

static int button(int id, int x, int y, int w, int h, const WCHAR *label, int on)
{
    int hv = hit(x, y, w, h);
    if (hv) g_hotId = id;
    int click = hv && M.pressed;
    if (click) M.pressed = 0;
    fill(x, y, w, h, on ? C_ACCENT : (hv ? C_PANEL2 : C_PANEL));
    frame(x, y, w, h, on ? C_ACCENTH : (hv ? C_BORDERH : C_BORDER));
    text(x, y, w, h, label, f_val, on ? RGB(14, 14, 12) : (hv ? C_ACCENTH : C_TEXT), DT_CENTER | DT_VCENTER, 2);
    return click;
}

static int toggle(int id, int x, int y, int w, const WCHAR *label, int *v)
{
    int hv = hit(x, y, w, 22);
    int ro = ro_hover(id, hv);
    if (ro) hv = 0; else if (hv) g_hotId = id;
    int changed = 0;
    if (hv && M.pressed) { *v = !*v; M.pressed = 0; changed = 1; }
    fill(x, y + 3, 16, 16, C_PANEL2);
    frame(x, y + 3, 16, 16, hv ? C_BORDERH : C_BORDER);
    if (*v) fill(x + 4, y + 7, 8, 8, ro ? C_LOCK : C_ACCENTH);
    text(x + 26, y, w - 26, 22, label, f_body, ro ? C_DIM : (*v ? C_TEXT : C_DIM), DT_LEFT | DT_VCENTER, 1);
    return changed;
}

static int slider(int id, int x, int y, int w, int labelW, const WCHAR *label,
                  float *v, float lo, float hi, float step, const WCHAR *fmt, float shown_mul)
{
    int tx = x + labelW, tw = w - labelW - 58;
    int hv = hit(tx - 6, y, tw + 12, 26);
    int ro = ro_hover(id, hv);
    if (ro) hv = 0; else if (hv) g_hotId = id;
    float old = *v;
    if (hv && M.pressed) { g_activeId = id; M.pressed = 0; }
    if (!ro && g_activeId == id && M.down) {
        float f = ((float)M.x - S(tx)) / (float)(S(tx + tw) - S(tx));
        *v = lo + tc_clampf(f, 0.f, 1.f) * (hi - lo);
    }
    if (hv && M.wheel) { *v += (M.wheel > 0 ? step : -step); M.wheel = 0; }
    if (step > 0) *v = roundf(*v / step) * step;
    *v = tc_clampf(*v, lo, hi);

    text(x, y, labelW, 26, label, f_body, C_DIM, DT_LEFT | DT_VCENTER, 1);
    int ty = y + 11;
    fill(tx, ty, tw, 4, C_PANEL2);
    frame(tx, ty, tw, 4, C_BORDER);
    float f = (*v - lo) / (hi - lo);
    int fw = (int)(tw * f);
    fill(tx, ty, fw, 4, ro ? C_LOCK : ((g_activeId == id || hv) ? C_ACCENTH : C_ACCENT));
    for (int i = 1; i < 4; i++) fill(tx + tw * i / 4, ty + 6, 1, 3, C_BORDER);
    if (!ro) fill(tx + fw - 3, y + 6, 6, 14, (g_activeId == id || hv) ? C_ACCENTH : C_TEXT);
    else     fill(tx + fw - 1, y + 8, 2, 10, C_LOCK);
    WCHAR b[32];
    swprintf(b, 32, fmt, *v * shown_mul);
    text(x + w - 52, y, 52, 26, b, f_val, ro ? C_DIM : C_TEXT, DT_RIGHT | DT_VCENTER, 0);
    return *v != old;
}

static int segmented(int id, int x, int y, int w, const WCHAR **labels, int n, int *v)
{
    int changed = 0, bw = w / n;
    for (int i = 0; i < n; i++) {
        int bx = x + i * bw, ww = (i == n - 1) ? w - bw * (n - 1) : bw;
        int hv = hit(bx, y, ww, 28);
        int ro = ro_hover(id + i, hv);
        if (ro) hv = 0; else if (hv) g_hotId = id + i;
        if (hv && M.pressed) { *v = i; M.pressed = 0; changed = 1; }
        int on = (*v == i);
        COLORREF on_bg = ro ? C_LOCK : C_ACCENT;
        fill(bx, y, ww, 28, on ? on_bg : (hv ? C_PANEL2 : C_PANEL));
        frame(bx, y, ww, 28, on ? (ro ? C_BORDERH : C_ACCENTH) : C_BORDER);
        text(bx, y, ww, 28, labels[i], f_val, on ? RGB(14, 14, 12) : (ro ? C_DIM : C_TEXT), DT_CENTER | DT_VCENTER, 2);
    }
    return changed;
}

static void key_name(int vk, WCHAR *out, int n)
{
    switch (vk) {
    case 0:           wcsncpy(out, L"NONE", n); return;
    case VK_SHIFT:    case VK_LSHIFT: case VK_RSHIFT: wcsncpy(out, L"SHIFT", n); return;
    case VK_CONTROL:  case VK_LCONTROL: case VK_RCONTROL: wcsncpy(out, L"CTRL", n); return;
    case VK_MENU:     case VK_LMENU: case VK_RMENU: wcsncpy(out, L"ALT", n); return;
    case VK_XBUTTON1: wcsncpy(out, L"MOUSE 4", n); return;
    case VK_XBUTTON2: wcsncpy(out, L"MOUSE 5", n); return;
    case VK_MBUTTON:  wcsncpy(out, L"MOUSE 3", n); return;
    }
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(sc << 16);
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
        lp |= 1 << 24;
    }
    if (!GetKeyNameTextW(lp, out, n)) swprintf(out, n, L"KEY %d", vk);
    CharUpperW(out);
}

static void keybind(int id, int x, int y, int w, int labelW, const WCHAR *label, int *vk)
{
    int bx = x + labelW, bw = w - labelW;
    int hv = hit(bx, y, bw, 28);
    if (hv) g_hotId = id;
    if (hv && M.pressed) { g_capture = vk; g_captureId = id; M.pressed = 0; SetFocus(g_ui); }
    int cap = (g_capture == vk && g_captureId == id);
    text(x, y, labelW, 28, label, f_body, C_DIM, DT_LEFT | DT_VCENTER, 1);
    fill(bx, y, bw, 28, cap ? C_PANEL2 : C_PANEL);
    frame(bx, y, bw, 28, cap ? C_ACCENTH : (hv ? C_BORDERH : C_BORDER));
    WCHAR b[64];
    if (cap) wcscpy(b, L"PRESS…");
    else key_name(*vk, b, 64);
    text(bx + 2, y, bw - 4, 28, b, f_val, cap ? C_ACCENTH : C_TEXT, DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS, cap ? 0 : 2);
}

/* ---------- shape tile icons ---------- */

static void build_icons(void)
{
    for (int s = 0; s < SH_COUNT; s++)
        for (int k = 0; k < 2; k++)
            if (g_iconBmp[s][k]) { DeleteObject(g_iconBmp[s][k]); g_iconBmp[s][k] = NULL; }
    if (!g_iconDC) g_iconDC = CreateCompatibleDC(NULL);
    g_iconW = S(46);
    for (int s = 0; s < SH_COUNT; s++) {
        for (int k = 0; k < 2; k++) {
            CrossParams p = { s, 11.f * g_scale, (float)(int)lroundf(fmaxf(1.f, 2.f * g_scale)),
                              3.f * g_scale, 1.f, 0, 0, 0, 0, k ? 0xE6DAA0 : 0xA8A48E };
            if (s == SH_DOT) p.size = 16.f * g_scale;
            if (s == SH_BOX || s == SH_CIRCLE) p.size = 9.f * g_scale;
            if (s == SH_RETICLE) { p.size = 8.f * g_scale; p.gap = 1.f * g_scale; }
            uint32_t *bits;
            g_iconBmp[s][k] = make_dib(g_iconW, g_iconW, &bits);
            tc_render(&p, 0, 0, bits, g_iconW, g_iconW);
        }
    }
}

static void blit_alpha(HDC src, int dx, int dy, int w, int h, BYTE a)
{
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, a, AC_SRC_ALPHA };
    AlphaBlend(g_dc, dx, dy, w, h, src, 0, 0, w, h, bf);
}

/* ---------- live preview ---------- */

static void draw_preview(int x, int y, int w, int h)
{
    int X = S(x), Y = S(y), W = S(x + w) - X, H = S(y + h) - Y;
    TRIVERTEX tv[2] = {
        { X, Y, 0x2C00, 0x2E00, 0x2700, 0xFF00 },
        { X + W, Y + H, 0x0D00, 0x0E00, 0x0C00, 0xFF00 },
    };
    GRADIENT_RECT gr = { 0, 1 };
    GradientFill(g_dc, tv, 2, &gr, 1, GRADIENT_FILL_RECT_V);
    /* faint range grid + horizon */
    for (int i = 1; i < 8; i++) {
        fillpx(X + W * i / 8, Y, 1, H, RGB(34, 36, 31));
        fillpx(X, Y + H * i / 8, W, 1, RGB(34, 36, 31));
    }
    fillpx(X, Y + H * 9 / 16, W, 1, RGB(60, 62, 52));
    /* distant tree line silhouette */
    for (int i = 0; i < W; i += S(3)) {
        int th = (int)((sinf(i * 0.045f) * 0.5f + sinf(i * 0.13f + 1.3f) * 0.3f + 0.9f) * S(14));
        fillpx(X + i, Y + H * 9 / 16 - th, S(3), th, RGB(22, 24, 20));
    }
    frame(x, y, w, h, C_BORDER);
    brackets(x - 4, y - 4, w + 8, h + 8, 14, C_ACCENT);

    /* click & hold inside the preview = test fire */
    g_testFire = g_set.recoilOn && M.down && hit(x, y, w, h) && g_activeId == 0;
    if (hit(x, y, w, h)) { g_hotId = 50; tip(50, L"Click (hold for full-auto) to test recoil.  Hold A / D / W / Space to test sway."); }

    /* crosshair (true screen size) with live sway + recoil */
    if (g_ovBmp) {
        int ix, iy, qx, qy;
        float sx, sy;
        total_offset(&sx, &sy);
        split_offset(sx, &ix, &qx);
        split_offset(sy, &iy, &qy);
        select_variant(qy * 4 + qx);
        int cx = X + W / 2 + ix - g_ovW / 2, cy = Y + H / 2 + iy - g_ovW / 2;
        HRGN clip = CreateRectRgn(X + 1, Y + 1, X + W - 1, Y + H - 1);
        SelectClipRgn(g_dc, clip);
        float a = g_set.enabled ? ((g_set.hideAim && g_aiming && !g_uiFg) ? 0.15f : 1.f) : 0.25f;
        blit_alpha(g_ovDC, cx, cy, g_ovW, g_ovW, (BYTE)(a * 255));
        SelectClipRgn(g_dc, NULL);
        DeleteObject(clip);
    }
    text(x + 10, y + h - 26, w - 20, 18,
         g_set.recoilOn ? L"A / D / W = SWAY    |    CLICK = FIRE" : L"HOLD  A / D / W  TO TEST TARKOVSWAY",
         f_small, C_DIM, DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS, 1);
}

static void status_row(int x, int y, int w, const WCHAR *k, const WCHAR *v, COLORREF c)
{
    text(x, y, w, 20, k, f_small, C_DIM, DT_LEFT | DT_VCENTER, 3);
    fill(x + w - 130, y + 7, 7, 7, c);
    text(x + w - 118, y, 118, 20, v, f_val, C_TEXT, DT_LEFT | DT_VCENTER, 2);
}

static void settings_changed(int crosshair)
{
    if (g_set.swayPreset > 0 && g_set.swayPreset <= PRESET_SLOTS)   /* slots save automatically */
        g_slot[g_set.swayPreset - 1] = preset_current();
    if (crosshair) crosshair_rebuild();
    g_saveAt = now_s() + 0.6;
    g_lastVer = -1;
}

static void reset_all(void)
{
    int tray = g_set.startTray;
    settings_defaults(&g_set);
    g_set.startTray = tray;
    for (int i = 0; i < PRESET_SLOTS; i++) g_slot[i] = PRESET_TARKOV;
    preset_apply(&PRESET_TARKOV);
    settings_changed(1);
}

static void quit_app(void);

/* Preset row (shared by the TarkovSway and Recoil tabs). Presets hold sway,
 * recoil and spread together, so one slot can describe one gun. */
static void presets_row(int CX, int CW, int *py)
{
    int y = *py;
    heading(CX, y, CW, L"PRESET  (SWAY + RECOIL)");
    y += 22;
        {
            static const WCHAR *names[1 + PRESET_SLOTS] = { L"TARKOV DEFAULT", L"SLOT 1", L"SLOT 2", L"SLOT 3" };
            int firstW = 152, restW = (CW - firstW) / PRESET_SLOTS;
            for (int i = 0; i <= PRESET_SLOTS; i++) {
                int bx = i == 0 ? CX : CX + firstW + (i - 1) * restW;
                int bw = i == 0 ? firstW : (i == PRESET_SLOTS ? CX + CW - bx : restW);
                int used = i == 0 || g_slotUsed[i - 1];
                int hv = hit(bx, y, bw, 28), on = g_set.swayPreset == i;
                if (hv) g_hotId = 230 + i;
                if (hv && M.pressed) {
                    M.pressed = 0;
                    g_set.swayPreset = i;
                    const Preset *p = preset_selected();
                    if (p) { preset_apply(p); tc_sway_reset(&g_sway); tc_recoil_reset(&g_recoil); }
                    settings_changed(1);
                }
                fill(bx, y, bw, 28, on ? C_ACCENT : (hv ? C_PANEL2 : C_PANEL));
                frame(bx, y, bw, 28, on ? C_ACCENTH : C_BORDER);
                text(bx, y, bw, 28, names[i], f_val, on ? RGB(14, 14, 12) : (used ? C_TEXT : C_DIM),
                     DT_CENTER | DT_VCENTER, 2);
            }
            tip(230, L"Built-in, laser-calibrated sway + recoil. Locked - it can never be edited.");
            tip(231, L"Your slot - starts as a copy of Tarkov Default. Changes save automatically.");
            tip(232, L"Your slot - starts as a copy of Tarkov Default. Changes save automatically.");
            tip(233, L"Your slot - starts as a copy of Tarkov Default. Changes save automatically.");
            y += 34;

            WCHAR st[96];
            int slot = g_set.swayPreset;
            if (slot == 0) {
                note(CX, y + 1, CW - 170, 30, L"LOCKED - pick a slot to adjust. Slots start as copies of this.", C_DIM);
                if (button(240, CX + CW - 160, y, 160, 28, L"CUSTOMIZE", 0)) {
                    g_set.swayPreset = 1;
                    preset_apply(&g_slot[0]);
                    tc_sway_reset(&g_sway); tc_recoil_reset(&g_recoil);
                    settings_changed(1);
                }
                tip(240, L"Switch to SLOT 1 so you can adjust the sliders.");
            } else {
                int same = preset_equal(&g_slot[slot - 1], &PRESET_TARKOV);
                swprintf(st, 96, same ? L"Slot %d - matches Tarkov Default. Changes save automatically."
                                      : L"Slot %d - customized. Changes save automatically.", slot);
                note(CX, y + 1, CW - 170, 30, st, same ? C_DIM : C_ACCENTH);
                if (button(241, CX + CW - 160, y, 160, 28, L"RESET TO DEFAULT", 0)) {
                    g_slot[slot - 1] = PRESET_TARKOV;
                    preset_apply(&PRESET_TARKOV);
                    tc_sway_reset(&g_sway); tc_recoil_reset(&g_recoil);
                    settings_changed(1);
                }
                tip(241, L"Copy Tarkov Default back into this slot.");
            }
            y += 34;
        }
    *py = y;
}

/* ---------- the whole UI, drawn and handled each paint ---------- */

static void ui_frame(void)
{
    g_hotId = 0;
    g_tip[0] = 0;
    g_ro = 0;
    RECT all = { 0, 0, g_backW, g_backH };
    SetDCBrushColor(g_dc, C_BG);
    FillRect(g_dc, &all, (HBRUSH)GetStockObject(DC_BRUSH));
    SetBkMode(g_dc, TRANSPARENT);

    /* ----- header ----- */
    fill(0, 0, UI_W, 50, C_HEAD);
    {   /* hazard hatch */
        HBRUSH hb = CreateHatchBrush(HS_BDIAGONAL, RGB(30, 29, 24));
        SetBkColor(g_dc, C_HEAD); SetBkMode(g_dc, OPAQUE);
        RECT r = { S(520), S(10), S(UI_W - 120), S(40) };
        FillRect(g_dc, &r, hb);
        DeleteObject(hb);
        SetBkMode(g_dc, TRANSPARENT);
    }
    fill(0, 50, UI_W, 1, C_BORDER);
    if (g_iconBmp[SH_RETICLE][1]) {
        HGDIOBJ o = SelectObject(g_iconDC, g_iconBmp[SH_RETICLE][1]);
        blit_alpha(g_iconDC, S(8), S(2), g_iconW, g_iconW, 255);
        SelectObject(g_iconDC, o);
    }
    text(58, 6, 200, 26, L"TARKOV", f_title, C_ACCENTH, DT_LEFT | DT_VCENTER, 4);
    text(160, 6, 200, 26, L"CROSSHAIR", f_title, C_TEXT, DT_LEFT | DT_VCENTER, 4);
    text(58, 30, 300, 16, L"OVERLAY UTILITY  //  v" APP_VERSION, f_small, C_DIM, DT_LEFT | DT_VCENTER, 3);
    {
        int hv = hit(UI_W - 96, 0, 48, 50);
        if (hv) { g_hotId = 9001; fill(UI_W - 96, 0, 48, 50, C_PANEL2); }
        fill(UI_W - 79, 25, 14, 2, hv ? C_ACCENTH : C_TEXT);
        if (hv && M.pressed) { M.pressed = 0; ShowWindow(g_ui, SW_MINIMIZE); }
        tip(9001, L"Minimize");
        hv = hit(UI_W - 48, 0, 48, 50);
        if (hv) { g_hotId = 9002; fill(UI_W - 48, 0, 48, 50, C_RED); }
        text(UI_W - 48, 0, 48, 50, L"X", f_val, hv ? RGB(255, 255, 255) : C_TEXT, DT_CENTER | DT_VCENTER, 0);
        if (hv && M.pressed) { M.pressed = 0; ShowWindow(g_ui, SW_HIDE); }
        tip(9002, L"Close to tray - the crosshair keeps running. Quit from the tray icon or Options.");
    }

    /* ----- left column: preview + status ----- */
    const int LX = 22, LW = 326;
    heading(LX, 66, LW, L"LIVE PREVIEW");
    draw_preview(LX, 92, LW, 364);

    int sy = 472;
    const WCHAR *ovs; COLORREF ovc;
    if (!g_set.enabled) { ovs = L"OFF"; ovc = C_RED; }
    else if (g_ovShown) { ovs = L"ACTIVE"; ovc = C_GREEN; }
    else { ovs = L"HIDDEN"; ovc = C_AMBER; }
    status_row(LX, sy, LW, L"OVERLAY", ovs, ovc);
    {
        WCHAR ds[48];
        swprintf(ds, 48, L"%dP  @%dHZ", g_screenH, g_frameHz);
        status_row(LX, sy + 22, LW, L"DISPLAY", ds, C_DIM);
    }
    status_row(LX, sy + 44, LW, L"STATE", g_inventory ? L"INVENTORY" : (g_aiming && !g_uiFg ? L"AIMING" : L"HIP"), (g_inventory || (g_aiming && !g_uiFg)) ? C_AMBER : C_DIM);
    fill(LX, sy + 72, LW, 1, C_BORDER);

    {
        WCHAR kn[32], lb[96];
        key_name(g_set.keyToggle, kn, 32);
        swprintf(lb, 96, g_set.enabled ? L"CROSSHAIR  ON" : L"CROSSHAIR  OFF");
        if (button(10, LX, 558, LW, 42, lb, g_set.enabled)) { g_set.enabled = !g_set.enabled; settings_changed(0); }
        swprintf(lb, 96, L"Show / hide anytime with  %ls", kn);
        tip(10, lb);
        swprintf(lb, 96, L"HOTKEY  %ls", kn);
        text(LX, 604, LW, 18, lb, f_small, C_DIM, DT_CENTER | DT_VCENTER, 3);
    }

    /* ----- right column: tabs ----- */
    const int RX = 374, RW = 464;
    int actX = RX, actW = 0;
    {
        const WCHAR *tabs[4] = { L"CROSSHAIR", L"TARKOVSWAY", L"RECOIL", L"OPTIONS" };
        /* size each tab to its label, then share the spare width evenly */
        int track = 2, lw[4], sum = 0;
        for (;;) {
            sum = 0;
            SelectObject(g_dc, f_val);
            SetTextCharacterExtra(g_dc, track ? S((float)track * 0.5f) : 0);
            for (int i = 0; i < 4; i++) {
                SIZE sz;
                GetTextExtentPoint32W(g_dc, tabs[i], (int)wcslen(tabs[i]), &sz);
                lw[i] = (int)ceilf((float)sz.cx / g_scale) + 1;
                sum += lw[i];
            }
            if (sum + 4 * 12 <= RW || track == 0) break;
            track--;                                   /* tighten spacing if it still doesn't fit */
        }
        int pad = (RW - sum) / 4, tx = RX;
        for (int i = 0; i < 4; i++) {
            int ww = i == 3 ? RX + RW - tx : lw[i] + pad;
            int hv = hit(tx, 62, ww, 36);
            if (hv) g_hotId = 20 + i;
            if (hv && M.pressed) { g_tab = i; M.pressed = 0; }
            int on = g_tab == i;
            fill(tx, 62, ww, 37, on ? C_PANEL : (hv ? RGB(16, 16, 14) : C_BG));
            if (on) { actX = tx; actW = ww; }
            if (on) {                                  /* active tab joins the panel below */
                fill(tx, 62, ww, 2, C_ACCENTH);
                fill(tx, 62, 1, 37, C_BORDER);
                fill(tx + ww - 1, 62, 1, 37, C_BORDER);
            } else if (i > 0 && g_tab != i - 1) {
                fill(tx, 72, 1, 16, C_BORDER);         /* subtle divider */
            }
            text(tx, 62, ww, 36, tabs[i], f_val, on ? C_ACCENTH : (hv ? C_TEXT : C_DIM), DT_CENTER | DT_VCENTER, track);
            tx += ww;
        }
    }
    fill(RX, 98, RW, 518, C_PANEL);
    frame(RX, 98, RW, 518, C_BORDER);
    if (actW > 2) fill(actX + 1, 98, actW - 2, 1, C_PANEL);   /* open the border under the active tab */
    const int CX = RX + 18, CW = RW - 36;
    int y = 114;

    if (g_tab == 0) {
        WCHAR hb[64];
        swprintf(hb, 64, L"SHAPE  —  %hs", TC_SHAPE_NAMES[g_set.xh.shape]);
        heading(CX, y, CW, hb);
        y += 24;
        for (int s = 0; s < SH_COUNT; s++) {
            int tx = CX + s * 54;
            int hv = hit(tx, y, 48, 48), on = g_set.xh.shape == s;
            if (hv) { g_hotId = 100 + s; }
            if (hv && M.pressed) { g_set.xh.shape = s; M.pressed = 0; settings_changed(1); }
            fill(tx, y, 48, 48, on ? C_PANEL2 : C_BG);
            frame(tx, y, 48, 48, on ? C_ACCENTH : (hv ? C_BORDERH : C_BORDER));
            HGDIOBJ o = SelectObject(g_iconDC, g_iconBmp[s][on || hv]);
            blit_alpha(g_iconDC, S(tx + 1), S(y + 1), g_iconW, g_iconW, 255);
            SelectObject(g_iconDC, o);
        }
        y += 64;

        heading(CX, y, CW, L"COLOR");
        y += 24;
        static const uint32_t sw[7] = { 0x3CFF6E, 0xFF3B3B, 0x3BE8FF, 0xFFE14D, 0xFFFFFF, 0xFF4DF0, 0xD8CFA0 };
        for (int i = 0; i < 7; i++) {
            int tx = CX + i * 38;
            int hv = hit(tx, y, 32, 30), on = g_set.xh.color == sw[i];
            if (hv) g_hotId = 120 + i;
            if (hv && M.pressed) { g_set.xh.color = sw[i]; M.pressed = 0; settings_changed(1); }
            uint32_t c = sw[i];
            frame(tx, y, 32, 30, on ? C_ACCENTH : (hv ? C_BORDERH : C_BORDER));
            fill(tx + 3, y + 3, 26, 24, RGB((c >> 16) & 255, (c >> 8) & 255, c & 255));
        }
        if (button(130, CX + 274, y, 96, 30, L"CUSTOM…", 0)) PostMessageW(g_ui, WM_APP_COLOR, 0, 0);
        {
            WCHAR hx[16];
            swprintf(hx, 16, L"#%06X", g_set.xh.color & 0xFFFFFF);
            text(CX + 372, y, CW - 372, 30, hx, f_small, C_DIM, DT_RIGHT | DT_VCENTER, 2);
        }
        y += 46;

        heading(CX, y, CW, L"DIMENSIONS");
        y += 22;
        int ch = 0;
        ch |= slider(140, CX, y, CW, 120, L"SIZE", &g_set.xh.size, 1, 60, 1, L"%.0f px", 1); y += 30;
        ch |= slider(141, CX, y, CW, 120, L"THICKNESS", &g_set.xh.thick, 1, 10, 1, L"%.0f px", 1); y += 30;
        ch |= slider(142, CX, y, CW, 120, L"GAP", &g_set.xh.gap, 0, 30, 1, L"%.0f px", 1); y += 30;
        ch |= slider(143, CX, y, CW, 120, L"OPACITY", &g_set.xh.opacity, 0.1f, 1, 0.05f, L"%.0f%%", 100); y += 38;

        heading(CX, y, CW, L"DETAILS");
        y += 22;
        ch |= toggle(150, CX, y + 2, 140, L"CENTER DOT", &g_set.xh.dot);
        ch |= slider(151, CX + 160, y, CW - 160, 58, L"SIZE", &g_set.xh.dotSize, 1, 10, 1, L"%.0f px", 1); y += 32;
        ch |= toggle(152, CX, y + 2, 140, L"OUTLINE", &g_set.xh.outline);
        ch |= slider(153, CX + 160, y, CW - 160, 58, L"WIDTH", &g_set.xh.outlineW, 0.5f, 3, 0.5f, L"%.1f px", 1); y += 36;
        if (ch) settings_changed(1);
        note(CX, y, CW, 36, L"Sizes are true screen pixels. Scroll over a slider for fine steps.", C_DIM);
    }
    else if (g_tab == 1) {
        int ch = 0;
        ch |= toggle(200, CX, y, CW, L"ENABLE TARKOVSWAY", &g_set.swayOn);
        tip(200, L"Crosshair drifts with your PMC's movement inertia: leans against your strafe and settles like a weapon.");
        if (ch) tc_sway_reset(&g_sway);
        y += 32;

        presets_row(CX, CW, &y);

        heading(CX, y, CW, L"TUNING");
        y += 22;
        const int LW2 = 172;
        g_ro = (g_set.swayPreset == 0);
        ch |= slider(210, CX, y, CW, LW2, L"INTENSITY", &g_set.swayInt, 0, 2, 0.05f, L"%.0f%%", 100); tip(210, L"Size of the swing and bob (does not change Strafe Lean)."); y += 26;
        ch |= slider(215, CX, y, CW, LW2, L"STRAFE LEAN", &g_set.swayLeanPx, 0, 80, 0.5f, L"%.1f px", 1); tip(215, L"Exact on-screen pixels the crosshair shifts while you hold A or D. Calibrate: hold A, fire at a wall, raise until the shot lands on the crosshair."); y += 26;
        ch |= slider(218, CX, y, CW, LW2, L"FORWARD DIP", &g_set.swayFwdPx, 0, 60, 0.5f, L"%.1f px", 1); tip(218, L"How far the barrel drops while you walk forward (W). 22 px measured against an in-game laser."); y += 26;
        ch |= slider(219, CX, y, CW, LW2, L"BACKWARD RISE", &g_set.swayBackPx, 0, 60, 0.5f, L"%.1f px", 1); tip(219, L"How far the barrel lifts while you walk backward (S). 12 px measured against an in-game laser."); y += 26;
        ch |= slider(217, CX, y, CW, LW2, L"RESPONSE DELAY", &g_set.swayDelayMs, 0, 400, 5, L"%.0f ms", 1); tip(217, L"How long after pressing A / D the weapon starts to swing. Tarkov's weapon trails your keys - 235 ms was measured against an in-game laser."); y += 26;
        ch |= slider(216, CX, y, CW, LW2, L"WIGGLE DAMPING", &g_set.swayWiggle, 0, 1, 0.05f, L"%.0f%%", 100); tip(216, L"Quick A-D-A-D wiggles swing less than full strides. 0% = off, 100% = strongest. Long strides are never reduced."); y += 26;
        ch |= slider(211, CX, y, CW, LW2, L"INERTIA / KIT WEIGHT", &g_set.swayInertia, 0, 1, 0.05f, L"%.0f%%", 100); tip(211, L"Higher = heavier loadout: slower, lazier swing - like being overweight in raid."); y += 26;
        ch |= slider(212, CX, y, CW, LW2, L"WALK BOB", &g_set.swayBob, 0, 2, 0.05f, L"%.0f%%", 100); tip(212, L"Figure-eight step bob while moving. Bigger and faster while sprinting."); y += 26;
        ch |= slider(213, CX, y, CW, LW2, L"MOUSE-LOOK SWAY", &g_set.swayMouse, 0, 2, 0.05f, L"%.0f%%", 100); tip(213, L"Weapon lag when turning. 0% = off (uses no extra input reading)."); y += 28;
        g_ro = 0;
        ch |= toggle(214, CX, y, CW, L"HIDE CROSSHAIR WHILE SPRINTING", &g_set.hideSprint); tip(214, L"You can't shoot while sprinting in Tarkov anyway."); y += 30;
        if (ch) settings_changed(0);

        heading(CX, y, CW, L"MOVEMENT KEYS");
        y += 22;
        {
            int cw3 = (CW - 24) / 3;
            keybind(220, CX, y, cw3, 60, L"LEFT", &g_set.kLeft);
            keybind(221, CX + cw3 + 12, y, cw3, 60, L"RIGHT", &g_set.kRight);
            keybind(222, CX + 2 * (cw3 + 12), y, CW - 2 * (cw3 + 12), 60, L"FWD", &g_set.kFwd); y += 32;
            keybind(223, CX, y, cw3, 60, L"BACK", &g_set.kBack);
            keybind(224, CX + cw3 + 12, y, cw3, 60, L"SPRINT", &g_set.kSprint);
            keybind(225, CX + 2 * (cw3 + 12), y, CW - 2 * (cw3 + 12), 60, L"JUMP", &g_set.kJump);
            for (int i = 220; i <= 225; i++)
                tip(i, L"Match your Tarkov binds. Click a box, then press the new key (Esc cancels).");
        }
    }
    else if (g_tab == 2) {
        int ch = 0, rebuild = 0;
        ch |= toggle(400, CX, y, CW, L"ENABLE RECOIL", &g_set.recoilOn);
        tip(400, L"Crosshair kicks up with each shot (left mouse) and settles back - shows where the barrel is pointing. Visual only.");
        if (ch) tc_recoil_reset(&g_recoil);
        y += 32;
        presets_row(CX, CW, &y);

        g_ro = (g_set.swayPreset == 0);
        heading(CX, y, CW, L"FIRE  (LEFT MOUSE)");
        y += 22;
        text(CX, y, 110, 28, L"FIRE MODE", f_body, C_DIM, DT_LEFT | DT_VCENTER, 1);
        static const WCHAR *fm[2] = { L"SEMI", L"FULL AUTO" };
        ch |= segmented(410, CX + 110, y, CW - 110, fm, 2, &g_set.fireMode);
        if (!g_ro) {
            tip(410, L"One kick per click.");
            tip(411, L"Kicks repeatedly at the fire rate while you hold left mouse.");
        }
        y += 34;
        ch |= slider(412, CX, y, CW, 150, L"FIRE RATE", &g_set.fireRpm, 300, 1200, 10, L"%.0f", 1);
        tip(412, L"Rounds per minute for full auto - match your gun (e.g. M4A1 800, AK-74M 650, MP7 950).");
        y += 32;

        heading(CX, y, CW, L"KICK");
        y += 22;
        ch |= slider(420, CX, y, CW, 150, L"VERTICAL", &g_set.kickV, 0, 12, 0.25f, L"%.2f px", 1); tip(420, L"How far one shot lifts the crosshair (at 1080p)."); y += 30;
        ch |= slider(421, CX, y, CW, 150, L"HORIZONTAL", &g_set.kickH, 0, 6, 0.25f, L"%.2f px", 1); tip(421, L"Random side-to-side wander per shot."); y += 30;
        ch |= slider(422, CX, y, CW, 150, L"MAX CLIMB", &g_set.maxClimb, 0, 80, 1, L"%.0f px", 1); tip(422, L"Where a long spray levels off - Tarkov guns stabilise after the first few rounds."); y += 30;
        ch |= slider(423, CX, y, CW, 150, L"RECOVERY", &g_set.recovery, 0, 1, 0.05f, L"%.0f%%", 100); tip(423, L"How fast the crosshair returns once you stop firing."); y += 32;

        rebuild |= toggle(430, CX, y + 2, 170, L"DYNAMIC SPREAD", &g_set.spreadOn);
        tip(430, L"Gap opens while you move and fire, like Tarkov's hip-fire spread. Works with every shape that has a gap.");
        rebuild |= slider(431, CX + 180, y, CW - 180, 70, L"AMOUNT", &g_set.spreadAmt, 0, 2, 0.05f, L"%.0f%%", 100);
        g_ro = 0;
        y += 34;
        if (rebuild) settings_changed(1);
        else if (ch) settings_changed(0);
        note(CX, y, CW, 34, L"Visual only: the crosshair shows where the barrel kicks - your mouse is never moved. Click the preview to test.", C_DIM);
    }
    else {
        int ch = 0;
        heading(CX, y, CW, L"AIMING");
        y += 22;
        ch |= toggle(300, CX, y, CW, L"HIDE CROSSHAIR WHEN AIMING  (RIGHT MOUSE)", &g_set.hideAim); y += 32;
        text(CX, y, 110, 28, L"AIM MODE", f_body, C_DIM, DT_LEFT | DT_VCENTER, 1);
        static const WCHAR *modes[3] = { L"HOLD", L"TOGGLE", L"TARKOV" };
        ch |= segmented(310, CX + 110, y, CW - 110, modes, 3, &g_set.aimMode);
        tip(310, L"Hidden only while right mouse is held down.");
        tip(311, L"Each right-click flips aiming on / off.");
        tip(312, L"Tap = toggle, hold = hold. Matches Tarkov's default ADS. Tab / Esc / sprint drop out of aim.");
        y += 34;
        {
            static const WCHAR *desc[3] = {
                L"Hidden only while right mouse is held.",
                L"Each right-click toggles aim on / off.",
                L"Tap to toggle, hold to hold - same as Tarkov's default ADS.",
            };
            note(CX + 110, y, CW - 110, 32, desc[g_set.aimMode], C_DIM);
        }
        y += 34;

        heading(CX, y, CW, L"VISIBILITY");
        y += 22;
        keybind(321, CX, y, CW, 200, L"SHOW / HIDE HOTKEY", &g_set.keyToggle); y += 34;
        ch |= toggle(323, CX, y, CW, L"HIDE IN INVENTORY  (TAB)", &g_set.hideInv);
        tip(323, L"Tab hides the crosshair until Tab or Esc closes the inventory. Press Tab again if it ever gets out of step.");
        y += 28;
        ch |= toggle(322, CX, y, CW, L"START MINIMIZED TO TRAY", &g_set.startTray); y += 30;
        tip(322, L"The crosshair stays on screen over every window until you hide it.");
        y += 4;

        heading(CX, y, CW, L"POSITION");
        y += 22;
        float ox = (float)g_set.offX, oy = (float)g_set.offY;
        if (slider(330, CX, y, CW, 120, L"OFFSET X", &ox, -50, 50, 1, L"%+.0f px", 1)) { g_set.offX = (int)ox; ch = 1; } y += 30;
        if (slider(331, CX, y, CW, 120, L"OFFSET Y", &oy, -50, 50, 1, L"%+.0f px", 1)) { g_set.offY = (int)oy; ch = 1; } y += 38;
        if (ch) settings_changed(0);

        int bw = (CW - 16) / 3;
        if (button(340, CX, y, bw, 34, L"RESET POSITION", 0)) { g_set.offX = g_set.offY = 0; settings_changed(0); }
        if (button(341, CX + bw + 8, y, bw, 34, L"RESET ALL", 0)) reset_all();
        if (button(342, CX + 2 * (bw + 8), y, CW - 2 * (bw + 8), 34, L"QUIT APP", 0)) PostMessageW(g_ui, WM_APP_QUIT, 0, 0);
        y += 46;
        note(CX, y, CW, 34, L"Run Tarkov in BORDERLESS mode - overlays can't draw over exclusive fullscreen.", C_AMBER);
    }

    /* tooltip line */
    fill(0, UI_H - 18, UI_W, 18, C_HEAD);
    text(14, UI_H - 18, UI_W - 28, 18, g_tip[0] ? g_tip : L"Close (X) keeps TarkovCrosshair running in the system tray.", f_small, g_tip[0] ? C_ACCENTH : C_DIM, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS, 1);

    M.pressed = 0;
    M.wheel = 0;
}

/* ================================================================== */
/* Settings window                                                     */
/* ================================================================== */

static void ui_resources(void)
{
    HFONT *fs[] = { &f_title, &f_head, &f_body, &f_small, &f_val, &f_big };
    for (int i = 0; i < 6; i++) if (*fs[i]) DeleteObject(*fs[i]);
    f_title = mkfont(23, FW_SEMIBOLD);
    f_head  = mkfont(12, FW_SEMIBOLD);
    f_body  = mkfont(13.5f, FW_NORMAL);
    f_small = mkfont(11.5f, FW_NORMAL);
    f_val   = mkfont(13, FW_SEMIBOLD);
    f_big   = mkfont(16, FW_SEMIBOLD);
    build_icons();
}

static void ensure_backbuffer(HDC ref, int w, int h)
{
    if (g_backBmp && g_backW == w && g_backH == h) return;
    if (!g_dc) g_dc = CreateCompatibleDC(ref);
    if (g_backBmp) { SelectObject(g_dc, g_backOld); DeleteObject(g_backBmp); }
    g_backBmp = CreateCompatibleBitmap(ref, w, h);
    g_backOld = (HBITMAP)SelectObject(g_dc, g_backBmp);
    g_backW = w; g_backH = h;
}

static void tray_add(void)
{
    memset(&g_nid, 0, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_ui;
    g_nid.uID = TRAY_ID;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_iconSmall;
    wcscpy(g_nid.szTip, L"TarkovCrosshair");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void show_ui(void)
{
    ShowWindow(g_ui, IsIconic(g_ui) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_ui);
    InvalidateRect(g_ui, NULL, FALSE);
}

static void quit_app(void)
{
    settings_save();
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    DestroyWindow(g_ui);
}

static void tray_menu(void)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_OPEN, L"Open TarkovCrosshair");
    AppendMenuW(m, MF_STRING | (g_set.enabled ? MF_CHECKED : 0), IDM_TOGGLE, L"Crosshair enabled");
    AppendMenuW(m, MF_STRING | (g_set.swayOn ? MF_CHECKED : 0), IDM_SWAY, L"TarkovSway");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Quit");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_ui);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0, g_ui, NULL);
    DestroyMenu(m);
    switch (cmd) {
    case IDM_OPEN:   show_ui(); break;
    case IDM_TOGGLE: g_set.enabled = !g_set.enabled; settings_changed(0); break;
    case IDM_SWAY:   g_set.swayOn = !g_set.swayOn; tc_sway_reset(&g_sway); settings_changed(0); break;
    case IDM_EXIT:   quit_app(); break;
    }
}

static UINT get_dpi(HWND h)
{
    typedef UINT (WINAPI *PFN)(HWND);
    static PFN fn;
    static int tried;
    if (!tried) { tried = 1; fn = (PFN)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"); }
    if (fn && h) return fn(h);
    HDC dc = GetDC(NULL);
    UINT d = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    return d;
}

static LRESULT CALLBACK ui_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    static UINT s_taskbarCreated;
    switch (msg) {
    case WM_CREATE:
        s_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        g_scale = get_dpi(h) / 96.f;
        ui_resources();
        return 0;
    case WM_DPICHANGED: {
        g_scale = HIWORD(wp) / 96.f;
        RECT *r = (RECT *)lp;
        ui_resources();
        SetWindowPos(h, NULL, r->left, r->top, S(UI_W), S(UI_H), SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(h, NULL, FALSE);
        return 0; }
    case WM_NCHITTEST: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(h, &p);
        if (p.y < S(50) && p.x < S(UI_W - 96)) return HTCAPTION;
        return HTCLIENT; }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT cr;
        GetClientRect(h, &cr);
        ensure_backbuffer(dc, cr.right, cr.bottom);
        if (g_ovBmp) ui_frame();
        if (IsWindow(h))
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
                   ps.rcPaint.bottom - ps.rcPaint.top, g_dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        EndPaint(h, &ps);
        return 0; }
    case WM_MOUSEMOVE: {
        M.x = GET_X_LPARAM(lp); M.y = GET_Y_LPARAM(lp);
        TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        InvalidateRect(h, NULL, FALSE);
        return 0; }
    case WM_MOUSELEAVE:
        M.x = M.y = -10000;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        if (g_capture) { g_capture = NULL; }
        M.down = 1; M.pressed = 1;
        M.x = GET_X_LPARAM(lp); M.y = GET_Y_LPARAM(lp);
        SetCapture(h);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP:
        M.down = 0;
        g_activeId = 0;
        ReleaseCapture();
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_MOUSEWHEEL:
        M.wheel = GET_WHEEL_DELTA_WPARAM(wp);
        {   POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(h, &p); M.x = p.x; M.y = p.y; }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_XBUTTONDOWN:
    case WM_MBUTTONDOWN:
        if (g_capture) {
            *g_capture = msg == WM_MBUTTONDOWN ? VK_MBUTTON : (GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2);
            g_capture = NULL;
            settings_changed(0);
            InvalidateRect(h, NULL, FALSE);
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (g_capture) {
            if (wp != VK_ESCAPE) {
                int vk = (int)wp;
                if (vk == VK_LSHIFT || vk == VK_RSHIFT) vk = VK_SHIFT;
                if (vk == VK_LCONTROL || vk == VK_RCONTROL) vk = VK_CONTROL;
                if (vk == VK_LMENU || vk == VK_RMENU) vk = VK_MENU;
                *g_capture = vk;
                settings_changed(0);
            }
            g_capture = NULL;
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_CLOSE) { ShowWindow(h, SW_HIDE); return 0; }
        break;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_INPUT: {
        RAWINPUT ri;
        UINT sz = sizeof ri;
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            g_mouseDx += (float)ri.data.mouse.lLastX;
            g_mouseDy += (float)ri.data.mouse.lLastY;
        }
        break; }
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) show_ui();
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) tray_menu();
        return 0;
    case WM_DISPLAYCHANGE:
        update_center();
        crosshair_rebuild();
        g_lastVer = -1;
        g_dirtyUI = 1;
        return 0;
    case WM_APP_QUIT:
        quit_app();
        return 0;
    case WM_APP_COLOR: {
        static COLORREF custom[16];
        CHOOSECOLORW cc = { sizeof cc };
        cc.hwndOwner = h;
        cc.lpCustColors = custom;
        uint32_t c = g_set.xh.color;
        cc.rgbResult = RGB((c >> 16) & 255, (c >> 8) & 255, c & 255);
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        M.down = 0; g_activeId = 0;
        if (ChooseColorW(&cc)) {
            g_set.xh.color = ((uint32_t)GetRValue(cc.rgbResult) << 16) |
                             ((uint32_t)GetGValue(cc.rgbResult) << 8) | GetBValue(cc.rgbResult);
            settings_changed(1);
        }
        InvalidateRect(h, NULL, FALSE);
        return 0; }
    case WM_COPYDATA: /* second instance asked us to show */
        show_ui();
        return TRUE;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        if (msg == s_taskbarCreated && s_taskbarCreated) { tray_add(); return 0; }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ================================================================== */
/* Icons                                                               */
/* ================================================================== */

static HICON make_icon(int sz)
{
    HICON ic = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, sz, sz, LR_DEFAULTCOLOR);
    if (ic) return ic;
    /* fallback: draw one */
    uint32_t *bits;
    HBITMAP col = make_dib(sz, sz, &bits);
    CrossParams p = { SH_RETICLE, sz * 0.22f, fmaxf(1.f, sz / 14.f), sz * 0.05f, 1, 1, fmaxf(1.f, sz / 10.f), 0, 0, 0xD6CA98 };
    tc_render(&p, 0, 0, bits, sz, sz);
    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, NULL);
    ICONINFO ii = { TRUE, 0, 0, mask, col };
    ic = CreateIconIndirect(&ii);
    DeleteObject(col); DeleteObject(mask);
    return ic;
}

/* ================================================================== */
/* Entry                                                               */
/* ================================================================== */

static void set_dpi_awareness(void)
{
    typedef BOOL (WINAPI *PFN)(HANDLE);
    PFN fn = (PFN)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (fn && fn((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */)) return;
    SetProcessDPIAware();
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev; (void)cmd; (void)show;
    g_inst = inst;

    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\TarkovCrosshair.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"TarkovCrosshairUI", NULL);
        if (other) {
            COPYDATASTRUCT cds = { 1, 0, NULL };
            SendMessageW(other, WM_COPYDATA, 0, (LPARAM)&cds);
        }
        return 0;
    }

    set_dpi_awareness();
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcFreq = (double)f.QuadPart;

    /* settings live in %APPDATA%\TarkovCrosshair\settings.ini */
    WCHAR dir[MAX_PATH];
    if (GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH)) {
        wcscat(dir, L"\\TarkovCrosshair");
        CreateDirectoryW(dir, NULL);
        swprintf(g_iniPath, MAX_PATH, L"%ls\\settings.ini", dir);
    } else {
        wcscpy(g_iniPath, L"settings.ini");
    }
    settings_load();
    slots_load();

    g_iconBig = make_icon(GetSystemMetrics(SM_CXICON));
    g_iconSmall = make_icon(GetSystemMetrics(SM_CXSMICON));

    /* overlay window: click-through, never focusable, no taskbar entry */
    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = L"TarkovCrosshairOverlay";
    RegisterClassExW(&wc);
    g_ov = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           wc.lpszClassName, L"TarkovCrosshair Overlay", WS_POPUP, 0, 0, 8, 8, NULL, NULL, inst, NULL);
    g_ovDC = CreateCompatibleDC(NULL);
    update_center();
    tc_recoil_reset(&g_recoil);
    crosshair_rebuild();

    /* settings window */
    WNDCLASSEXW uc = { sizeof uc };
    uc.lpfnWndProc = ui_proc;
    uc.hInstance = inst;
    uc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    uc.hIcon = g_iconBig;
    uc.hIconSm = g_iconSmall;
    uc.lpszClassName = L"TarkovCrosshairUI";
    RegisterClassExW(&uc);
    g_ui = CreateWindowExW(WS_EX_APPWINDOW, uc.lpszClassName, APP_NAME,
                           WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN,
                           CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, NULL, NULL, inst, NULL);
    {
        POINT o = { 0, 0 };
        MONITORINFO mi = { sizeof mi };
        GetMonitorInfoW(MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY), &mi);
        int w = S(UI_W), hh = S(UI_H);
        SetWindowPos(g_ui, NULL, (mi.rcWork.left + mi.rcWork.right - w) / 2,
                     (mi.rcWork.top + mi.rcWork.bottom - hh) / 2, w, hh, SWP_NOZORDER);
    }
    tray_add();
    if (!g_set.startTray) show_ui();

    update_center();
    check_foreground();

    /* high-resolution waitable timer drives the loop; falls back to normal */
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(NULL, FALSE, NULL);

    LARGE_INTEGER due;
    due.QuadPart = -10000;
    SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
    for (;;) {
        DWORD r = MsgWaitForMultipleObjectsEx(1, &timer, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) goto done;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (r == WAIT_OBJECT_0) {           /* timer fired: run one tick, re-arm */
            double waitMs = tick();
            if (g_saveAt > 0 && now_s() >= g_saveAt) { settings_save(); g_saveAt = 0; }
            due.QuadPart = -(LONGLONG)(waitMs * 10000.0);
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
        }
    }
done:
    raw_mouse(0);
    settings_save();
    cache_free();
    if (mutex) CloseHandle(mutex);
    return 0;
}
