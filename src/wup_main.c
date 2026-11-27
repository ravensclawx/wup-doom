/* wup-doom: Doom on the Wii U GamePad, for wup-homebrew 2 (libwup). doomgeneric's engine; this file is the pad's side
 * of it: the start, the controls, the picture and the error screen. Sound and music: wup_sound.c.
 *
 * The picture: Doom draws 320 x 200 in 8 bits a pixel, as the pad's 8-bit screen takes them: no colour conversion,
 * the palette goes straight to the LCD. Shown 4:3 as on a CRT: 640 x 480, each pixel twice across, each row two or
 * three times down (480 / 200 = 2.4), centred.
 *
 * The controls (G_BuildTiccmd): the left stick moves (forward and back analog, through Doom's mouse; sideways
 * strafes), the right stick turns (analog, as a mouse). ZR fires, A uses (and confirms in the menus), ZL runs,
 * L / R change weapon, B goes back in the menus, + is the menu, - the map, Y answers yes, X no, the D-pad moves in the
 * menus and the game. Home goes back to the Homebrew Menu. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wup.h"
#include "gfx.h"

#include "doomgeneric.h"
#include "doomkeys.h"
#include "d_event.h"
#include "i_video.h"
#include "m_argv.h"
#include "m_controls.h"
#include "v_video.h"
#include "z_zone.h"
#include "w_wad.h"
#include "i_system.h"
#include "d_main.h"
#include "tables.h"
#include "doomstat.h"
#include "p_pspr.h"

WUP_APP("wup-doom");

/* ---- the picture */
#define OUT_W 640
#define OUT_X ((854 - OUT_W) / 2)
byte *I_VideoBuffer;
boolean screenvisible = true, screensaver_mode = false;
int usegamma, usemouse = 1, vanilla_keyboard_mapping = 1;
float mouse_acceleration = 2.0;
int mouse_threshold = 10;
static uint8_t *scr;
static unsigned stride;
static uint16_t row_of[480];                         /* the source row of each screen row */

void I_InitGraphics(void) {
    I_VideoBuffer = Z_Malloc(SCREENWIDTH * SCREENHEIGHT, PU_STATIC, NULL);
    memset(I_VideoBuffer, 0, SCREENWIDTH * SCREENHEIGHT);   /* (the first screen wipe melts from it: left as it came,
                                                              it showed a frame of garbage) */
    scr = wup_screen8(); stride = wup_stride8();
    memset(scr, 0, stride * 480);
    for (int y = 0; y < 480; y++) row_of[y] = (uint16_t)(y * SCREENHEIGHT / 480);
    I_SetPalette(W_CacheLumpName("PLAYPAL", PU_CACHE));   /* (Doom's own, from the start) */
    wup_present8(0, 480);
}
void I_ShutdownGraphics(void) {}
void I_FinishUpdate(void) {
    const byte *src = I_VideoBuffer;
    int last = -1;
    uint8_t *prev = NULL;
    for (int y = 0; y < 480; y++) {
        uint8_t *d = scr + y * stride + OUT_X;
        int r = row_of[y];
        if (r == last) { memcpy(d, prev, OUT_W); continue; }   /* (the row repeated: copied) */
        const byte *s = src + r * SCREENWIDTH;
        uint32_t *d32 = (uint32_t *)d;                         /* (OUT_X and the stride keep it word aligned) */
        for (int x = 0; x < SCREENWIDTH; x += 2) {
            uint32_t a = s[x], b = s[x + 1];
            *d32++ = a | a << 8 | b << 16 | b << 24;            /* each pixel twice */
        }
        last = r; prev = d;
    }
    wup_present8(0, 480);
}
void I_SetPalette(byte *pal) {                       /* (gamma: Doom's own table, as the PC version does it) */
    for (int i = 0; i < 256; i++, pal += 3)
        wup_palette(i, (uint32_t)gammatable[usegamma][pal[0]] << 16 | (uint32_t)gammatable[usegamma][pal[1]] << 8 | gammatable[usegamma][pal[2]]);
}
int I_GetPaletteIndex(int r, int g, int b) {
    byte *pal = W_CacheLumpName("PLAYPAL", PU_CACHE);
    int best = 0, best_d = 1 << 30;
    for (int i = 0; i < 256; i++, pal += 3) {
        int d = (r - pal[0]) * (r - pal[0]) + (g - pal[1]) * (g - pal[1]) + (b - pal[2]) * (b - pal[2]);
        if (d < best_d) { best = i; best_d = d; if (!d) break; }
    }
    return best;
}
void I_ReadScreen(byte *dst) { memcpy(dst, I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT); }
void I_UpdateNoBlit(void) {}
void I_BeginRead(void) {}
void I_EndRead(void) {}
void I_SetWindowTitle(char *title) { (void)title; }
void I_GraphicsCheckCommandLine(void) {}
void I_SetGrabMouseCallback(grabmouse_callback_t f) { (void)f; }
void I_EnableLoadingDisk(void) {}
void I_BindVideoVariables(void) {}
void I_DisplayFPSDots(boolean on) { (void)on; }
void I_CheckIsScreensaver(void) {}
void I_StartFrame(void) {}
void I_InitInput(void) {}

/* ---- the controls, every tic */
static const struct { unsigned bit; int key, key2; } map[] = {
    { WUP_ZR, KEY_FIRE, 0 }, { WUP_A, KEY_USE, KEY_ENTER }, { WUP_ZL, KEY_RSHIFT, 0 },
    { WUP_L, '[', 0 }, { WUP_R, ']', 0 },             /* (the previous / next weapon: set at the start) */
    { WUP_B, KEY_BACKSPACE, 0 }, { WUP_PLUS, KEY_ESCAPE, 0 }, { WUP_MINUS, KEY_TAB, 0 },
    { WUP_Y, 'y', 0 }, { WUP_X, 'n', 0 },
    { WUP_UP, KEY_UPARROW, 0 }, { WUP_DOWN, KEY_DOWNARROW, 0 }, { WUP_LEFT, KEY_LEFTARROW, 0 }, { WUP_RIGHT, KEY_RIGHTARROW, 0 },
};
static unsigned held;
static void key(int k, int down) {
    event_t ev = { down ? ev_keydown : ev_keyup, k, k < 128 ? k : 0, 0, 0 };
    D_PostEvent(&ev);
}
static int curve(int v, int full) {                  /* a stick: a dead zone, then finer near the middle */
    int a = abs(v);
    if (a < 120) return 0;
    if (a > 1100) a = 1100;
    a -= 120;
    long r = (long)a * a / 980 * full / 980;
    return v < 0 ? -(int)r : (int)r;
}
/* ---- rumble: the player's own shots (the weapon's muzzle flash appearing: every gun, not the monsters', which sound
   the same) and the hits the player takes (the red flash, Doom's damagecount: longer for harder hits) */
static uint32_t rumble_until;
static int rumbling;
static void rumble_for(int ms) {
    uint32_t until = DG_GetTicksMs() + (uint32_t)ms;
    if (!rumbling || (int)(until - rumble_until) > 0) rumble_until = until;
    if (!rumbling) { wup_rumble(1); rumbling = 1; }
}
static void rumble_tic(void) {
    static const void *flash_was;
    static int damage_was;
    if (gamestate == GS_LEVEL && !paused && !menuactive) {
        player_t *p = &players[consoleplayer];
        const void *flash = p->psprites[ps_flash].state;
        if (flash && !flash_was) {
            static const short ms[NUMWEAPONS] = { 0, 70, 130, 60, 140, 90, 260, 0, 130 };   /* fist .. super shotgun */
            if ((unsigned)p->readyweapon < NUMWEAPONS && ms[p->readyweapon]) rumble_for(ms[p->readyweapon]);
        }
        flash_was = flash;
        if (p->readyweapon == wp_chainsaw && p->attackdown) rumble_for(40);   /* (the chainsaw: while it saws) */
        if (p->damagecount > damage_was) rumble_for(60 + 8 * (p->damagecount - damage_was));
        damage_was = p->damagecount;
    } else flash_was = NULL;
    if (rumbling && (int)(DG_GetTicksMs() - rumble_until) >= 0) { wup_rumble(0); rumbling = 0; }
}

void I_StartTic(void) {
    rumble_tic();
    unsigned b = wup_buttons();
    if (b & WUP_HOME) I_Quit();                      /* (back to the Homebrew Menu) */
    for (unsigned i = 0; i < sizeof map / sizeof *map; i++) {
        unsigned m = map[i].bit;
        if ((b ^ held) & m) { key(map[i].key, (b & m) != 0); if (map[i].key2) key(map[i].key2, (b & m) != 0); }
    }
    held = b;
    /* the sticks, through Doom's mouse (analog: turning, forward and back) and joystick (strafing) */
    int lx = wup_stick(0, 0), ly = wup_stick(0, 1), rx = wup_stick(1, 0);
    event_t m = { ev_mouse, 0, curve(rx, 180), curve(ly, 50), 0 };
    D_PostEvent(&m);
    event_t j = { ev_joystick, 0, 0, 0, lx > 450 ? 1 : lx < -450 ? -1 : 0 };
    D_PostEvent(&j);
}

/* ---- doomgeneric's platform calls (its own loop and screen buffer go unused) */
void DG_Init(void) {}
void DG_DrawFrame(void) {}
void DG_SleepMs(uint32_t ms) { wup_sleep((int)ms); }
uint32_t DG_GetTicksMs(void) {
    static uint32_t last, ms, frac;
    uint32_t now = wup_us(); frac += now - last; last = now; ms += frac / 1000; frac %= 1000;
    return ms;
}
int DG_GetKey(int *pressed, unsigned char *k) { (void)pressed; (void)k; return 0; }
void DG_SetWindowTitle(const char *t) { (void)t; }

/* ---- an error: on the screen, until a button */
void wupdoom_error(const char *msg) {
    wup_log("wup-doom: "); wup_log(msg); wup_log("\n");
    px_t *p = wup_screen();
    gfx_fill(0, 0, GFX_W, GFX_H, RGB(0, 0, 0));
    gfx_text_c(&font_big, GFX_W / 2, 200, "Doom Stopped", RGB(255, 255, 255));
    gfx_text_c(&font_body, GFX_W / 2, 250, msg, RGB(190, 190, 190));
    gfx_text_c(&font_body, GFX_W / 2, 440, "Press any button to return to the menu", RGB(150, 150, 150));
    gfx_present_all(); (void)p;
    while (wup_buttons()) wup_sleep(16);
    while (!wup_buttons()) wup_sleep(16);
}

void wupdoom_sound_start(void);                      /* wup_sound.c */
void D_DoomMain(void);
void doomgeneric_Tick(void);
int main(void) {
    static char *args[] = { "doom", "-iwad", "doom1.wad", NULL };   /* (relative: the app's folder, libwup; the config
                                                                    and saves go there too) */
    myargc = 3; myargv = args;
    key_prevweapon = '['; key_nextweapon = ']';
    wupdoom_sound_start();
    D_DoomMain();                                    /* (the start, and the first tic) */
    for (;;) doomgeneric_Tick();                     /* (I_Quit ends the app) */
}
