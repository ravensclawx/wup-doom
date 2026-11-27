/* wup-doom's sound: the sound effects and the music, mixed by one audio thread into libwup's speaker ring (48 kHz
 * stereo).
 *
 * The effects (doomgeneric's sound module: DG_sound_module): Doom's own 8-bit samples from the WAD (lumps DS*, mostly
 * 11025 Hz), on 8 channels, each stepped to 48 kHz in 16.16 fixed point and panned as Doom asks. A sound's lump stays
 * locked in the zone while a channel plays it (I_UpdateSound gives it back).
 *
 * The music: Chocolate Doom's OPL player (i_oplmusic.c: the WAD's MUS songs, as MIDI, played on an OPL2 with the
 * WAD's own instrument bank, GENMIDI: the sound of a Sound Blaster), on an emulated YM3812 (emu8950). This is the
 * player's OPL driver (opl_wup_driver, after Chocolate's opl_sdl.c): the chip makes its samples at its own rate
 * (49716 Hz, clock / 72: no rate converter, which needs floating point), stepped to 48 kHz here, and the player's
 * timer callbacks run at exactly the sample they fall on.
 *
 * Everything runs in the audio thread, which keeps about 40 ms queued. */
#include <stdlib.h>
#include <string.h>
#include "wup.h"

#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "deh_str.h"
#include "opl.h"
#include "opl_internal.h"
#include "opl_queue.h"
#include "emu8950.h"

#define RATE 48000
#define AHEAD (RATE * 40 / 1000)                       /* frames kept queued */
#define BLOCK 256

/* ---- locks (libwup semaphores) */
typedef struct { int s; } lock_t;
static void lock_init(lock_t *l) { l->s = wup_sem_new(1, 1); }
static void lock(lock_t *l) { wup_sem_wait(l->s, -1); }
static void unlock(lock_t *l) { wup_sem_signal(l->s); }

/* ================= the effects ================= */
#define NCHAN 8
typedef struct { const uint8_t *data; uint32_t len, pos, step; int vl, vr; sfxinfo_t *sfx; } chan_t;
static chan_t ch[NCHAN];
static lock_t ch_lock;
static boolean use_prefix;

static boolean Snd_Init(boolean prefix) { use_prefix = prefix; return true; }
static void Snd_Shutdown(void) {}
static int Snd_GetSfxLumpNum(sfxinfo_t *sfx) {
    char name[9];
    if (sfx->link) sfx = sfx->link;
    if (use_prefix) M_snprintf(name, sizeof name, "ds%s", DEH_String(sfx->name));
    else M_StringCopy(name, DEH_String(sfx->name), sizeof name);
    return W_GetNumForName(name);
}
static void params(chan_t *c, int vol, int sep) {      /* vol 0..127, sep 0 (left) .. 255 (right) */
    c->vl = vol * (254 - sep) / 127;                   /* (as Chocolate's SDL module: 0..254 each side) */
    c->vr = vol * sep / 127;
}
static void Snd_UpdateSoundParams(int c, int vol, int sep) {
    if (c < 0 || c >= NCHAN) return;
    lock(&ch_lock); params(&ch[c], vol, sep); unlock(&ch_lock);
}
static int Snd_StartSound(sfxinfo_t *sfx, int c, int vol, int sep) {
    if (c < 0 || c >= NCHAN) return -1;
    int lump = sfx->lumpnum;
    if (lump < 0) return -1;
    const uint8_t *d = W_CacheLumpNum(lump, PU_STATIC);   /* (locked while it plays: I_UpdateSound) */
    uint32_t n = W_LumpLength(lump);
    if (n < 8 || d[0] != 3 || d[1] != 0) { W_ReleaseLumpNum(lump); return -1; }   /* (format 3: DMX's PCM) */
    uint32_t rate = d[2] | d[3] << 8, len = d[4] | d[5] << 8 | d[6] << 16 | (uint32_t)d[7] << 24;
    if (len > n - 8) len = n - 8;
    if (len > 32) { d += 16; len -= 32; }              /* (DMX's 16 bytes of padding at each end) */
    else d += 8;
    lock(&ch_lock);
    chan_t *k = &ch[c];
    k->data = d + 8; k->len = len << 16; k->pos = 0; k->step = (uint32_t)(((uint64_t)rate << 16) / RATE);
    params(k, vol, sep); k->sfx = sfx;
    unlock(&ch_lock);
    return c;
}
static void Snd_StopSound(int c) {
    if (c < 0 || c >= NCHAN) return;
    lock(&ch_lock); ch[c].data = NULL; unlock(&ch_lock);
}
static boolean Snd_SoundIsPlaying(int c) { return c >= 0 && c < NCHAN && ch[c].data != NULL; }
/* the lumps of finished sounds back to the zone's cache (here, in the game's thread: the zone is not the audio
   thread's to touch) */
static sfxinfo_t *locked[NCHAN];
static void Snd_Update(void) {
    lock(&ch_lock);
    for (int i = 0; i < NCHAN; i++) {
        sfxinfo_t *now = ch[i].data ? ch[i].sfx : NULL;
        if (locked[i] != now) {
            sfxinfo_t *was = locked[i]; locked[i] = now;
            int still = 0;
            for (int k = 0; k < NCHAN; k++) still |= was && locked[k] == was;
            if (was && !still) W_ReleaseLumpNum(was->lumpnum);
        }
    }
    unlock(&ch_lock);
}
static void Snd_CacheSounds(sfxinfo_t *s, int n) { (void)s; (void)n; }
static snddevice_t snd_devices[] = { SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER,
                                     SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32 };
sound_module_t DG_sound_module = {
    snd_devices, (int)(sizeof snd_devices / sizeof *snd_devices),
    Snd_Init, Snd_Shutdown, Snd_GetSfxLumpNum, Snd_Update, Snd_UpdateSoundParams,
    Snd_StartSound, Snd_StopSound, Snd_SoundIsPlaying, Snd_CacheSounds,
};
static void mix_sfx(int32_t *lr, int n) {
    lock(&ch_lock);
    for (int i = 0; i < NCHAN; i++) {
        chan_t *c = &ch[i];
        if (!c->data) continue;
        for (int k = 0; k < n; k++) {
            int s = (int)c->data[c->pos >> 16] - 128;
            lr[2 * k] += s * c->vl; lr[2 * k + 1] += s * c->vr;    /* (+-128 x 254: up to ~32 K each) */
            c->pos += c->step;
            if (c->pos >= c->len) { c->data = NULL; break; }
        }
    }
    unlock(&ch_lock);
}

/* ================= the music: the OPL driver ================= */
typedef struct { unsigned rate, enabled, value; uint64_t expire_time; } opl_timer_t;   /* (as opl_sdl.c) */
#define CHIP_RATE 49716u                               /* 3579545 Hz / 72 */
static OPL *chip;
static lock_t cb_lock, q_lock;                          /* (the player's callbacks; the queue) */
static opl_callback_queue_t *queue;
static uint64_t now_us, pause_offset;
static int paused, reg_num;
static opl_timer_t timer1 = { 12500, 0, 0, 0 }, timer2 = { 3125, 0, 0, 0 };
static uint32_t step_frac;                             /* chip samples to output samples, 16.16 */
static int16_t chip_prev, chip_cur;

static void advance(unsigned n) {                      /* n output samples later: due callbacks run */
    lock(&q_lock);
    uint64_t us = (uint64_t)n * OPL_SECOND / RATE;
    now_us += us;
    if (paused) pause_offset += us;
    while (!OPL_Queue_IsEmpty(queue) && now_us >= OPL_Queue_Peek(queue) + pause_offset) {
        opl_callback_t cb; void *data;
        if (!OPL_Queue_Pop(queue, &cb, &data)) break;
        unlock(&q_lock);
        lock(&cb_lock); cb(data); unlock(&cb_lock);
        lock(&q_lock);
    }
    unlock(&q_lock);
}
static void chip_samples(int32_t *lr, unsigned n) {    /* n samples of the chip, at 48 kHz (linear between its own) */
    for (unsigned k = 0; k < n; k++) {
        step_frac += (CHIP_RATE << 16) / RATE;
        while (step_frac >= 0x10000) { step_frac -= 0x10000; chip_prev = chip_cur; chip_cur = OPL_calc(chip); }
        int32_t v = chip_prev + (((int32_t)chip_cur - chip_prev) * (int32_t)(step_frac >> 4) >> 12);
        v *= 2;                                         /* (the chip is quiet beside the effects) */
        lr[2 * k] += v; lr[2 * k + 1] += v;
    }
}
static void mix_music(int32_t *lr, unsigned n) {      /* as opl_sdl.c: up to each callback, then the callback */
    for (unsigned done = 0; done < n; ) {
        unsigned k = n - done;
        lock(&q_lock);
        if (!paused && !OPL_Queue_IsEmpty(queue)) {
            uint64_t next = OPL_Queue_Peek(queue) + pause_offset;
            uint64_t want = next > now_us ? ((next - now_us) * RATE + OPL_SECOND - 1) / OPL_SECOND : 0;
            if (want < k) k = (unsigned)want;
        }
        unlock(&q_lock);
        if (k) chip_samples(lr + 2 * done, k);
        advance(k);
        done += k;
        if (!k) advance(0);                              /* (a callback due now) */
    }
}

static int Opl_Init(unsigned int port) {
    (void)port;
    if (!chip) { chip = OPL_new(3579545, CHIP_RATE); lock_init(&cb_lock); lock_init(&q_lock); queue = OPL_Queue_Create(); }
    OPL_reset(chip);
    now_us = pause_offset = 0; paused = 0;
    return 1;
}
static void Opl_Shutdown(void) {}
static unsigned int Opl_PortRead(opl_port_t port) {
    unsigned r = 0;
    if (port == OPL_REGISTER_PORT_OPL3) return 0xff;   /* (an OPL2: no OPL3 here) */
    if (timer1.enabled && now_us > timer1.expire_time) r |= 0xc0;
    if (timer2.enabled && now_us > timer2.expire_time) r |= 0xa0;
    return r;
}
static void timer_end(opl_timer_t *t) { if (t->enabled) t->expire_time = now_us + (uint64_t)(0x100 - t->value) * OPL_SECOND / t->rate; }
static void Opl_PortWrite(opl_port_t port, unsigned int v) {
    if (port == OPL_REGISTER_PORT) { reg_num = (int)v; return; }
    if (port == OPL_REGISTER_PORT_OPL3) { reg_num = (int)v | 0x100; return; }
    switch (reg_num) {
    case OPL_REG_TIMER1: timer1.value = v; timer_end(&timer1); break;
    case OPL_REG_TIMER2: timer2.value = v; timer_end(&timer2); break;
    case OPL_REG_TIMER_CTRL:
        if (v & 0x80) { timer1.enabled = timer2.enabled = 0; break; }
        if (!(v & 0x40)) { timer1.enabled = (v & 1) != 0; timer_end(&timer1); }
        if (!(v & 0x20)) { timer2.enabled = (v & 2) != 0; timer_end(&timer2); }
        break;
    default:
        if (reg_num < 0x100) OPL_writeReg(chip, (uint32_t)reg_num, (uint8_t)v);   /* (an OPL2: the low bank only) */
    }
}
static void Opl_SetCallback(uint64_t us, opl_callback_t cb, void *data) {
    lock(&q_lock); OPL_Queue_Push(queue, cb, data, now_us - pause_offset + us); unlock(&q_lock);
}
static void Opl_ClearCallbacks(void) { lock(&q_lock); OPL_Queue_Clear(queue); unlock(&q_lock); }
static void Opl_Lock(void) { lock(&cb_lock); }
static void Opl_Unlock(void) { unlock(&cb_lock); }
static void Opl_SetPaused(int p) { paused = p; }
static void Opl_AdjustCallbacks(float f) { lock(&q_lock); OPL_Queue_AdjustCallbacks(queue, now_us, f); unlock(&q_lock); }
opl_driver_t opl_wup_driver = {
    "wup", Opl_Init, Opl_Shutdown, Opl_PortRead, Opl_PortWrite, Opl_SetCallback, Opl_ClearCallbacks,
    Opl_Lock, Opl_Unlock, Opl_SetPaused, Opl_AdjustCallbacks,
};

/* ================= the audio thread ================= */
static void audio_thread(void *arg) {
    (void)arg;
    static int32_t acc[2 * BLOCK];
    static int16_t out[2 * BLOCK];
    for (;;) {
        if (wup_audio_queued() >= AHEAD) { wup_thread_sleep(2); continue; }
        memset(acc, 0, sizeof acc);
        if (chip) mix_music(acc, BLOCK);
        mix_sfx(acc, BLOCK);
        for (int i = 0; i < 2 * BLOCK; i++) {
            int32_t v = acc[i] >> 1;                    /* (headroom for 8 channels and the music) */
            out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        for (int done = 0; done < BLOCK; ) done += wup_audio_queue(out + 2 * done, BLOCK - done);
    }
}
void wupdoom_sound_start(void) {
    lock_init(&ch_lock);
    if (wup_audio_open()) return;
    wup_thread_start(audio_thread, NULL, 8192, 8);    /* (above the game: sound never starves) */
}

/* (i_sound.c binds these config variables, SDL_mixer's resampler settings: unused here) */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;
