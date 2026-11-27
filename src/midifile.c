// (wup-doom: a MIDI reader for Chocolate Doom 3.1.0's OPL player, with midifile.h's interface. Chocolate's own reads
// a song into a table of every event, ~20 bytes each: E1M2's 43 KB song became ~200 KB, E1M8's ~350 KB, which the
// pad's memory could not spare at a level change. This one keeps the song as it is, the MIDI file's own bytes, and
// decodes each event when the player asks for it (as Chocolate's ReadEvent decodes it: running status, sysex, meta
// events with their data pointing into the song). Its memory is the song's size, from Doom's zone.)
//
// Copyright(C) 2005-2014 Simon Howard (the reading rules: Chocolate Doom's midifile.c)
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "midifile.h"
#include "z_zone.h"

typedef struct { const byte *data; unsigned int len; } track_t;

struct midi_file_s
{
    byte *song;                      // the whole MIDI file (zone)
    unsigned int division;           // the header's time division, as stored
    unsigned int num_tracks;
    track_t *tracks;
};

struct midi_track_iter_s
{
    const track_t *track;
    unsigned int pos;                // the next event's offset in the track
    unsigned int after;              // past the decoded event (when have)
    unsigned int running;            // the last status byte (running status)
    int have;                        // ev holds the event at pos
    int ended;                       // the end-of-track event has been given
    midi_event_t ev;
};

static unsigned int be16(const byte *p) { return p[0] << 8 | p[1]; }
static unsigned int be32(const byte *p) { return (unsigned int) p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// a variable-length number (at most 4 bytes); false past the end
static boolean VarLen(const track_t *t, unsigned int *pos, unsigned int *out)
{
    unsigned int v = 0;
    for (int i = 0; i < 4; ++i)
    {
        if (*pos >= t->len) return false;
        byte b = t->data[(*pos)++];
        v = v << 7 | (b & 0x7f);
        if (!(b & 0x80)) { *out = v; return true; }
    }
    return false;
}

// the event at iter->pos into iter->ev (iter->after: just past it)
static boolean Decode(midi_track_iter_t *iter)
{
    const track_t *t = iter->track;
    unsigned int p = iter->pos, status;
    midi_event_t *e = &iter->ev;

    if (!VarLen(t, &p, &e->delta_time) || p >= t->len) return false;
    status = t->data[p];
    if (status & 0x80) { ++p; iter->running = status; }   // (else: the previous status again)
    else status = iter->running;

    switch (status & 0xf0)
    {
        case MIDI_EVENT_NOTE_OFF:
        case MIDI_EVENT_NOTE_ON:
        case MIDI_EVENT_AFTERTOUCH:
        case MIDI_EVENT_CONTROLLER:
        case MIDI_EVENT_PITCH_BEND:
            if (p + 2 > t->len) return false;
            e->event_type = status & 0xf0;
            e->data.channel.channel = status & 0x0f;
            e->data.channel.param1 = t->data[p];
            e->data.channel.param2 = t->data[p + 1];
            p += 2;
            break;

        case MIDI_EVENT_PROGRAM_CHANGE:
        case MIDI_EVENT_CHAN_AFTERTOUCH:
            if (p + 1 > t->len) return false;
            e->event_type = status & 0xf0;
            e->data.channel.channel = status & 0x0f;
            e->data.channel.param1 = t->data[p];
            e->data.channel.param2 = 0;
            p += 1;
            break;

        case 0xf0:
            if (status == MIDI_EVENT_SYSEX || status == MIDI_EVENT_SYSEX_SPLIT)
            {
                e->event_type = status;
                if (!VarLen(t, &p, &e->data.sysex.length) || p + e->data.sysex.length > t->len) return false;
                e->data.sysex.data = (byte *) t->data + p;
                p += e->data.sysex.length;
                break;
            }
            if (status == MIDI_EVENT_META)
            {
                e->event_type = MIDI_EVENT_META;
                if (p >= t->len) return false;
                e->data.meta.type = t->data[p++];
                if (!VarLen(t, &p, &e->data.meta.length) || p + e->data.meta.length > t->len) return false;
                e->data.meta.data = (byte *) t->data + p;
                p += e->data.meta.length;
                break;
            }
            return false;

        default:
            return false;
    }

    iter->after = p;
    iter->have = 1;
    return true;
}

midi_file_t *MIDI_LoadMem(void *data, size_t len)
{
    const byte *d = data;
    midi_file_t *file;

    if (len < 14 || memcmp(d, "MThd", 4) || be32(d + 4) != 6)
    {
        fprintf(stderr, "MIDI_LoadMem: not a MIDI file\n");
        return NULL;
    }
    unsigned int format = be16(d + 8), ntracks = be16(d + 10);
    if ((format != 0 && format != 1) || ntracks < 1)
    {
        fprintf(stderr, "MIDI_LoadMem: format %u, %u tracks: not supported\n", format, ntracks);
        return NULL;
    }

    file = Z_Malloc(sizeof *file, PU_STATIC, NULL);
    file->song = Z_Malloc(len, PU_STATIC, NULL);
    memcpy(file->song, d, len);
    file->division = be16(d + 12);
    file->num_tracks = ntracks;
    file->tracks = Z_Malloc(sizeof(track_t) * ntracks, PU_STATIC, NULL);

    size_t at = 14;
    for (unsigned int i = 0; i < ntracks; ++i)
    {
        if (at + 8 > len || memcmp(file->song + at, "MTrk", 4) || be32(file->song + at + 4) > len - at - 8)
        {
            fprintf(stderr, "MIDI_LoadMem: track %u damaged\n", i);
            MIDI_FreeFile(file);
            return NULL;
        }
        file->tracks[i].len = be32(file->song + at + 4);
        file->tracks[i].data = file->song + at + 8;
        at += 8 + file->tracks[i].len;
    }
    return file;
}

midi_file_t *MIDI_LoadFile(char *filename)
{
    FILE *f = fopen(filename, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    byte *b = n > 0 ? malloc(n) : NULL;
    midi_file_t *m = b && fread(b, 1, n, f) == (size_t) n ? MIDI_LoadMem(b, n) : NULL;
    free(b);
    fclose(f);
    return m;
}

void MIDI_FreeFile(midi_file_t *file)
{
    if (file == NULL) return;
    if (file->tracks) Z_Free(file->tracks);
    if (file->song) Z_Free(file->song);
    Z_Free(file);
}

unsigned int MIDI_GetFileTimeDivision(midi_file_t *file)
{
    short result = (short) file->division;

    // Negative time division indicates SMPTE time and must be handled
    // differently.
    if (result < 0)
    {
        return (signed int)(-(result/256)) * (signed int)(result & 0xFF);
    }
    return result;
}

unsigned int MIDI_NumTracks(midi_file_t *file)
{
    return file->num_tracks;
}

midi_track_iter_t *MIDI_IterateTrack(midi_file_t *file, unsigned int track)
{
    midi_track_iter_t *iter = malloc(sizeof *iter);
    if (iter == NULL) return NULL;
    memset(iter, 0, sizeof *iter);
    iter->track = &file->tracks[track];
    return iter;
}

void MIDI_FreeIterator(midi_track_iter_t *iter)
{
    free(iter);
}

// the time until the next event (0 if there is none)
unsigned int MIDI_GetDeltaTime(midi_track_iter_t *iter)
{
    if (iter->ended) return 0;
    if (!iter->have && !Decode(iter)) return 0;
    return iter->ev.delta_time;
}

// the next event: 1, or 0 at the end of the track
int MIDI_GetNextEvent(midi_track_iter_t *iter, midi_event_t **event)
{
    if (iter->ended) return 0;
    if (!iter->have && !Decode(iter)) { iter->ended = 1; return 0; }
    *event = &iter->ev;
    iter->pos = iter->after;
    iter->have = 0;
    if (iter->ev.event_type == MIDI_EVENT_META && iter->ev.data.meta.type == MIDI_META_END_OF_TRACK)
    {
        iter->ended = 1;             // (given once; nothing after it)
    }
    return 1;
}

void MIDI_RestartIterator(midi_track_iter_t *iter)
{
    iter->pos = 0;
    iter->running = 0;
    iter->have = 0;
    iter->ended = 0;
}
