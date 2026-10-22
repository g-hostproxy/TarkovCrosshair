TARKOVCROSSHAIR v1.0
====================

A ~100 KB native Windows crosshair overlay with TarkovSway.
No installer, no dependencies (Windows 10/11).

QUICK START
-----------
1. Put TarkovCrosshair.exe anywhere and run it.
2. In Tarkov: Settings > Graphics > Screen mode = BORDERLESS.
   (No overlay can draw over exclusive Fullscreen.)
3. Pick a shape/colour/size. The crosshair sits in the centre of your
   main monitor over every window, all the time, until you hide it.
4. Closing the window (X) keeps it running in the system tray.
   Right-click the tray icon to toggle features or quit.

Settings are saved to %APPDATA%\TarkovCrosshair\settings.ini

FEATURES
--------
CROSSHAIR   8 shapes (cross, T, X, dot, circle, reticle, chevron, box),
            size, thickness, gap, opacity, centre dot, outline, any colour.
            Anti-aliased, but 1/3/5 px lines stay pixel-crisp.

AIM HIDE    Hides the crosshair on right mouse. Three modes:
              HOLD   - hidden while held
              TOGGLE - each click flips
              TARKOV - tap toggles, hold holds (matches the game's default ADS).
                       Tab / Esc / sprinting also drop out of "aim", like in game.

TARKOVSWAY  The crosshair hangs off your PMC on a spring, like Tarkov:
              hold A (strafe left)  -> barrel eases RIGHT and stays there
              hold D (strafe right) -> barrel eases LEFT and stays there
              A-D-A-D taps          -> right-left-right-left swing
              release               -> settles back to centre
            Strafe lean           - exact on-screen pixels it stays pushed
                                    while A/D is held (0-40 px, default 34).
                                    Calibrate per gun: hold A, fire single
                                    shots at a wall, raise the value until
                                    the impacts land on the crosshair.
            Presets               - TARKOV DEFAULT (built-in, in-game tuned):
                                    intensity 80%, strafe lean 34 px,
                                    inertia 80%, walk bob 120%,
                                    mouse-look 70%, hide while sprinting.
                                    Plus 3 slots: pick a slot, tune the
                                    sliders, press SAVE TO SLOT.
            Inertia / kit weight  - heavier = slower, wider, lazier swing.
            Walk bob              - figure-eight step bob, bigger when sprinting.
            Mouse-look sway       - lag when turning (off by default).
            Keys are rebindable - match them to your Tarkov binds.
            Test it in the preview: focus the window and hold A / D / W.

HOTKEY      Insert shows/hides the crosshair anywhere (rebindable).
            The app is not tied to any game: aim-hide and sway react to
            your keys/mouse in every program, so press Insert when you're
            done playing.

PERFORMANCE
-----------
- Single thread, sleeps on a high-resolution timer:
    switched off ........ 10 checks/sec
    on, standing still .. 125 Hz key polling, no drawing
    while swaying ....... updates at your refresh rate (max 165 Hz)
- The crosshair is pre-rendered; swaying only moves a tiny window.
- When hidden (aiming, toggled off) the overlay window is
  removed entirely, so it costs the compositor nothing.
- Expect ~0% CPU and a few MB of RAM.
- Note: on Windows 11, any always-on-top overlay (this, Discord, Steam,
  Crosshair X...) can stop a borderless game using "independent flip",
  which may add a tiny bit of latency. Toggle with Insert to compare.

ANTI-CHEAT - PLEASE READ
------------------------
TarkovCrosshair is built to be as passive as possible:
  - completely independent of the game: it never looks at, detects or
    talks to any other window or process
  - never opens, reads, writes or injects into anything
  - no keyboard/mouse hooks, no drivers, no input simulation
  - only reads which keys are held (GetAsyncKeyState)
  - draws in its own separate window; does not touch game files
  - visible to screenshots/recordings (nothing is hidden from BattlEye)

BUT: nobody can promise "no ban". Battlestate's Terms of Service forbid
third-party software that gives an advantage, and they decide what counts.
External crosshair overlays (Crosshair X, HudSight, etc.) are widely used
and generally reported as not detected or banned, but there are anecdotal
claims otherwise and BSG has never officially approved them. Use it at your
own risk, and avoid advertising that you use one in Tarkov.

TROUBLESHOOTING
---------------
Crosshair not visible in game  -> use Borderless; check the tray icon /
                                  press Insert. It draws at the centre of
                                  your PRIMARY monitor.
Aim-hide / sway don't react    -> run TarkovCrosshair as administrator
                                  (only needed if the game runs elevated).
Off-centre by a pixel          -> Options > Position offset.
Windows SmartScreen warning    -> the exe is unsigned; More info > Run anyway.

BUILDING FROM SOURCE
--------------------
src/core.h   crosshair rasterizer + sway physics (portable C)
src/main.c   Win32 overlay, input loop, settings UI
res/         icon, manifest, version info
build.bat    builds with MinGW-w64 / llvm-mingw:
             gcc -O2 -municode -mwindows src\main.c res.o -luser32 -lgdi32
                 -lshell32 -lcomdlg32 -lmsimg32
