/*
 *      Rhythm Game for STVN Engine - Win32s Port
 *      (c) 2026 Toyoyo
 *      Ported from Ren'Py rhythm game
 *
 *      Beatmap format: newline-separated timestamps (seconds)
 *      Input: Arrow keys (Left=track0, Up=track1, Down=track2, Right=track3)
 *
 *      Layout: black background, 4 white track lines, blue hit-zone bar near
 *      the top. Notes (green circles with white arrows) rise from the bottom
 *      of the screen toward the blue bar.
 */

#include "global.h"

/* External state from w3vn.c */
extern HWND g_hwnd;
extern volatile int g_running;
extern volatile int g_lastkey;
extern volatile int g_effectrunning;
extern uint32_t *g_videoram;
extern uint32_t *g_background;

/* External functions */
extern void RestoreWindowSize(void);
extern void update_display(void);

static float rg_fabsf(float x) { return (x < 0.0f) ? -x : x; }

static int rg_isqrt(int n) {
    if (n <= 0) return 0;
    int x = n, y = 1;
    while (x > y) { x = (x + y) / 2; y = n / x; }
    return x;
}

#define IsWine() (GetProcAddress(GetModuleHandle("ntdll.dll"), "wine_get_version") != NULL)

/* ── colours (BGRA) ─────────────────────────────────────────────────────── */
#define COLOR_BLUE      0xFF4DC8FF   /* hit-zone bar (unfilled) */
#define COLOR_DKBLUE    0xFF2A6E99   /* hit-zone bar (filled)   */
#define COLOR_GREEN     0xFF43D35A   /* left arrow note         */
#define COLOR_DKGREEN   0xFF2D8A3E   /* left arrow note outline */
#define COLOR_RED       0xFFFF8080   /* up arrow note           */
#define COLOR_DKRED     0xFF994C4C   /* up arrow note outline   */
#define COLOR_LTBLUE    0xFF00A5FF   /* down arrow note (pastel blue) */
#define COLOR_DKLTBLUE  0xFF006699   /* down arrow note outline     */
#define COLOR_ORANGE    0xFFffC882   /* right arrow note (pastel orange) */
#define COLOR_DKORANGE  0xFF98784E   /* right arrow note outline        */

/* ── layout ─────────────────────────────────────────────────────────────── */
#define NUM_TRACKS      4
#define X_OFFSET        50
#define TRACK_SPACING   ((SCREEN_WIDTH - 2 * X_OFFSET) / (NUM_TRACKS - 1))
#define TRACK_WIDTH     2            /* track-line width in px */

#define BAR_HEIGHT      14           /* height of blue bar    */
#define BAR_Y           (TEXT_AREA_START - BAR_HEIGHT - 5)  /* top of bar, near bottom */
#define BAR_MID_Y       (BAR_Y + BAR_HEIGHT / 2)

#define NOTE_RADIUS     16
#define NOTE_OFFSET_MS  3000         /* ms before beat note appears */
#define HIT_THRESHOLD   300          /* ms window for a hit         */

/* ── rhythm game state ───────────────────────────────────────────────────── */
typedef struct {
    float     *onset_times;
    int       *track_indices;
    int       *hit_status;
    int        num_notes;
    int        num_hits;
    int        score;
    int        combo;
    int        has_started;
    int        has_ended;
    DWORD      music_start_time;
    DWORD      mci_play_at;      /* when to actually start MCI (0 = already started) */
    UINT       mci_device_id;
    int        mci_playing;
    int        countdown;
    DWORD      countdown_start;
    uint32_t  *bg_pixels;    /* background image, NULL = black */
    HMIDIOUT   midi_out;     /* for hit ding sound (non-Wine), NULL = unavailable */
    DWORD      note_off_at;  /* when to send CC120 All Sound Off (0 = none pending) */
    UINT       mci_ding_id;  /* Wine: MCI MIDI device for hit ding */
    char       ding_tmp[260];/* Wine: temp MIDI file path */
} RhythmGame;

/* ── PRNG ────────────────────────────────────────────────────────────────── */
static unsigned int rg_seed = 12345;
static int rg_rand(int lo, int hi) {
    rg_seed = rg_seed * 1103515245u + 12345u;
    return lo + (int)((rg_seed >> 16) % (unsigned)(hi - lo + 1));
}

static unsigned int rg_hash_string(const char *s) {
    unsigned int h = 5381;
    while (*s) { h = h * 33 + (unsigned char)*s; s++; }
    return h;
}

/* ── pixel helpers ───────────────────────────────────────────────────────── */
static void rg_rect(int x, int y, int w, int h, uint32_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if (x + w > SCREEN_WIDTH)  w = SCREEN_WIDTH  - x;
    if (y + h > TEXT_AREA_START) h = TEXT_AREA_START - y;
    if (w <= 0 || h <= 0) return;
    for (int py = y; py < y + h; py++)
        for (int px = x; px < x + w; px++)
            g_videoram[py * SCREEN_WIDTH + px] = c;
}

static void rg_circle(int cx, int cy, int r, uint32_t c) {
    for (int dy = -r; dy <= r; dy++) {
        int w = rg_isqrt(r * r - dy * dy);
        int y = cy + dy;
        if (y < 0 || y >= TEXT_AREA_START) continue;
        int x1 = cx - w < 0 ? 0 : cx - w;
        int x2 = cx + w >= SCREEN_WIDTH ? SCREEN_WIDTH - 1 : cx + w;
        for (int x = x1; x <= x2; x++)
            g_videoram[y * SCREEN_WIDTH + x] = c;
    }
}

/* White directional arrow centred at (cx, cy) for the given track */
static void rg_arrow(int cx, int cy, int track) {
    int ah = 9;   /* arrowhead half-height */
    int sw = 4;   /* stem thickness */
    int sl = 8;   /* stem length */

    switch (track) {
        case 1: /* Up */
            for (int i = 0; i <= ah; i++)
                rg_rect(cx - i, cy - ah + i, 2*i+1, 1, COLOR_WHITE);
            rg_rect(cx - sw/2, cy, sw, sl, COLOR_WHITE);
            break;
        case 2: /* Down */
            for (int i = 0; i <= ah; i++)
                rg_rect(cx - i, cy + ah - i, 2*i+1, 1, COLOR_WHITE);
            rg_rect(cx - sw/2, cy - sl, sw, sl, COLOR_WHITE);
            break;
        case 0: /* Left */
            for (int i = 0; i <= ah; i++)
                rg_rect(cx - ah + i, cy - i, 1, 2*i+1, COLOR_WHITE);
            rg_rect(cx, cy - sw/2, sl, sw, COLOR_WHITE);
            break;
        case 3: /* Right */
            for (int i = 0; i <= ah; i++)
                rg_rect(cx + ah - i, cy - i, 1, 2*i+1, COLOR_WHITE);
            rg_rect(cx - sl, cy - sw/2, sl, sw, COLOR_WHITE);
            break;
    }
}

/* Green circle with white arrow */
static void rg_draw_note(int track, int y_center, int is_large) {
    int r = is_large ? NOTE_RADIUS * 5 / 4 : NOTE_RADIUS;
    int x  = X_OFFSET + track * TRACK_SPACING;
    uint32_t color, dkcolor;
    switch (track) {
        case 0: /* Left - green */
            color = COLOR_GREEN; dkcolor = COLOR_DKGREEN; break;
        case 1: /* Up - red */
            color = COLOR_RED; dkcolor = COLOR_DKRED; break;
        case 2: /* Down - blue */
            color = COLOR_LTBLUE; dkcolor = COLOR_DKLTBLUE; break;
        case 3: /* Right - orange */
            color = COLOR_ORANGE; dkcolor = COLOR_DKORANGE; break;
        default:
            color = COLOR_GREEN; dkcolor = COLOR_DKGREEN; break;
    }
    rg_circle(x, y_center, r + 2, dkcolor);
    rg_circle(x, y_center, r,     color);
    rg_arrow (x, y_center, track);
}

/* ── text helpers (uses g_font8x15 from func.c, same TU) ────────────────── */
static void rg_putc(int x, int y, char c, uint32_t fg) {
    unsigned char uc = (unsigned char)c;
    if (uc < 32) return;
    int idx = uc - 32;
    if (idx >= 224) return;
    for (int row = 0; row < 15; row++) {
        int py = y + row;
        if (py < 0 || py >= TEXT_AREA_START) continue;
        uint8_t g = g_font8x15[idx][row];
        for (int bit = 0; bit < 8; bit++) {
            int px = x + bit;
            if (px < 0 || px >= SCREEN_WIDTH) continue;
            if (g & (0x80 >> bit))
                g_videoram[py * SCREEN_WIDTH + px] = fg;
        }
    }
}

static void rg_puts(int x, int y, const char *s, uint32_t fg) {
    while (*s) { rg_putc(x, y, *s++, fg); x += 8; }
}

static void rg_puts_outlined(int x, int y, const char *s, uint32_t fg) {
    int dx, dy;
    for (dy = -1; dy <= 1; dy++)
        for (dx = -1; dx <= 1; dx++)
            if (dx != 0 || dy != 0)
                rg_puts(x + dx, y + dy, s, COLOR_BLACK);
    rg_puts(x, y, s, fg);
}

/* Draw a single character scaled up (each font pixel → scale×scale block) */
static void rg_putc_big(int x, int y, char c, int scale, uint32_t fg) {
    unsigned char uc = (unsigned char)c;
    if (uc < 32) return;
    int idx = uc - 32;
    if (idx >= 224) return;
    for (int row = 0; row < 15; row++) {
        uint8_t g = g_font8x15[idx][row];
        for (int bit = 0; bit < 8; bit++) {
            if (g & (0x80 >> bit))
                rg_rect(x + bit*scale, y + row*scale, scale, scale, fg);
        }
    }
}
static void rg_putc_big_outlined(int x, int y, char c, int scale, uint32_t fg) {
    int dx, dy;
    for (dy = -1; dy <= 1; dy++)
        for (dx = -1; dx <= 1; dx++)
            if (dx != 0 || dy != 0)
                rg_putc_big(x + dx, y + dy, c, scale, COLOR_BLACK);
    rg_putc_big(x, y, c, scale, fg);
}

/* ── beatmap loader ──────────────────────────────────────────────────────── */
/* stride: keep every Nth note (1 = all, 2 = every other, matching Ren'Py default) */
static int rg_load_beatmap(const char *path, RhythmGame *gm, int stride) {
    FILE *fp = fopen(path, "r");
    int n = 0, kept = 0, idx = 0, i = 0;
    char line[64];
    if (!fp) return -1;
    if (stride < 1) stride = 1;
    while (fgets(line, sizeof line, fp))
        if (line[0] > ' ') n++;
    if (n == 0) { fclose(fp); return -1; }

    /* count how many survive the stride */
    kept = (n + stride - 1) / stride;

    gm->onset_times = (float *)malloc(kept * sizeof(float));
    if (!gm->onset_times) { fclose(fp); return -1; }

    rewind(fp);
    while (fgets(line, sizeof line, fp) && idx < kept) {
        if (line[0] > ' ') {
            if (i % stride == 0)
                gm->onset_times[idx++] = (float)atof(line);
            i++;
        }
    }
    fclose(fp);
    gm->num_notes = idx;
    return 0;
}

/* ── MCI helpers ─────────────────────────────────────────────────────────── */
static void rg_start_music(RhythmGame *gm) {
    if (gm->mci_playing) return;
    DWORD play_time = timeGetTime() + NOTE_OFFSET_MS;
    gm->mci_playing = 1;
    /* Shift timeline back by NOTE_OFFSET_MS so first note starts at top of screen */
    gm->music_start_time = play_time;
    gm->mci_play_at      = play_time;
}

static int rg_music_playing(UINT id) {
    MCI_STATUS_PARMS s;
    memset(&s, 0, sizeof s);
    s.dwItem = MCI_STATUS_MODE;
    if (mciSendCommand(id, MCI_STATUS, MCI_STATUS_ITEM, (DWORD)(LPVOID)&s) == 0)
        return s.dwReturn == MCI_MODE_PLAY;
    return 0;
}

/* ── init ────────────────────────────────────────────────────────────────── */
static int rg_init(const char *bg, const char *audio, const char *bmap, int stride, RhythmGame *gm) {
    memset(gm, 0, sizeof *gm);
    if (rg_load_beatmap(bmap, gm, stride) != 0) return -1;

    gm->track_indices = (int *)malloc(gm->num_notes * sizeof(int));
    gm->hit_status    = (int *)malloc(gm->num_notes * sizeof(int));
    if (!gm->track_indices || !gm->hit_status) {
        free(gm->onset_times); free(gm->track_indices); free(gm->hit_status);
        return -1;
    }

    rg_seed = rg_hash_string(bmap);
    for (int i = 0; i < gm->num_notes; i++) {
        gm->track_indices[i] = rg_rand(0, NUM_TRACKS - 1);
        gm->hit_status[i]    = 0;
    }

    /* Load background image */
    if (bg && bg[0] != '\0') {
        gm->bg_pixels = (uint32_t *)malloc(IMAGE_AREA_PIXELS * sizeof(uint32_t));
        if (gm->bg_pixels) {
            uint8_t bgpalette[32];
            if (LoadBackgroundImage(bg, bgpalette, gm->bg_pixels) != 0) {
                free(gm->bg_pixels);
                gm->bg_pixels = NULL;
            }
        }
    }

    /* Resolve audio path */
    char fullpath[260];
    if (GetFullPathNameA(audio, 260, fullpath, NULL) == 0) {
        strncpy(fullpath, audio, 259); fullpath[259] = '\0';
    }
    if (GetFileAttributesA(fullpath) == INVALID_FILE_ATTRIBUTES) {
        free(gm->onset_times); free(gm->track_indices); free(gm->hit_status);
        free(gm->bg_pixels);
        return -1;
    }

    /* Open MCI (don't start yet – wait for countdown) */
    MCI_OPEN_PARMS mo;
    memset(&mo, 0, sizeof mo);
    mo.lpstrElementName = fullpath;
    mo.lpstrAlias       = "rythm_audio";
    DWORD flags = MCI_OPEN_ELEMENT | MCI_OPEN_ALIAS;

    /* So that the volume control may work */
    if (IsWine()) { mo.lpstrDeviceType = "mpegvideo"; flags |= MCI_OPEN_TYPE; }

    if (mciSendCommand(0, MCI_OPEN, flags, (DWORD)(LPVOID)&mo) != 0) {
        free(gm->onset_times); free(gm->track_indices); free(gm->hit_status);
        free(gm->bg_pixels);
        return -1;
    }
    gm->mci_device_id = mo.wDeviceID;
    /* Ensure position queries return milliseconds */
    MCI_SET_PARMS msp;
    memset(&msp, 0, sizeof msp);
    msp.dwTimeFormat = MCI_FORMAT_MILLISECONDS;
    mciSendCommand(gm->mci_device_id, MCI_SET, MCI_SET_TIME_FORMAT, (DWORD)(LPVOID)&msp);
    /* Wine: apply cached volume to the rhythm game MCI device */
    if (g_wineVolume >= 0) {
        char vcmd[128];
        snprintf(vcmd, sizeof(vcmd), "setaudio rythm_audio volume to %d", (g_wineVolume * 1000) / 100);
        mciSendString(vcmd, NULL, 0, NULL);
    }
    gm->has_started   = 1;
    gm->countdown     = 3;
    gm->countdown_start = timeGetTime();

    /* Open MIDI for hit ding */
    gm->midi_out    = NULL;
    gm->note_off_at = 0;
    gm->mci_ding_id = 0;
    gm->ding_tmp[0] = '\0';

    if (IsWine()) {
        /* Wine: midiOut doesn't route through FluidSynth; generate a tiny
           MIDI file and keep an MCI mpegvideo device open for seek+replay */
        static const unsigned char ding_mid[] = {
            /* MThd: format 0, 1 track, 480 ticks/quarter */
            0x4D,0x54,0x68,0x64, 0x00,0x00,0x00,0x06,
            0x00,0x00, 0x00,0x01, 0x01,0xE0,
            /* MTrk */
            0x4D,0x54,0x72,0x6B, 0x00,0x00,0x00,0x14,
            /* delta=0, tempo 500000us (120 BPM) */
            0x00, 0xFF,0x51,0x03, 0x07,0xA1,0x20,
            /* delta=0, note-on ch10 note49 vel127 */
            0x00, 0x99,0x31,0x7F,
            /* delta=480 ticks (500ms), note-off ch10 note49 vel0 */
            0x83,0x60, 0x99,0x31,0x00,
            /* delta=0, end of track */
            0x00, 0xFF,0x2F,0x00
        };
        char tmpdir[260];
        GetTempPathA(sizeof(tmpdir), tmpdir);
        snprintf(gm->ding_tmp, sizeof(gm->ding_tmp), "%sding_rythm.mid", tmpdir);
        FILE *f = fopen(gm->ding_tmp, "wb");
        if (f) {
            fwrite(ding_mid, 1, sizeof(ding_mid), f);
            fclose(f);
            char fullpath[260];
            if (GetFullPathNameA(gm->ding_tmp, sizeof(fullpath), fullpath, NULL) == 0)
                strncpy(fullpath, gm->ding_tmp, 259);
            MCI_OPEN_PARMS mo;
            memset(&mo, 0, sizeof mo);
            mo.lpstrElementName = fullpath;
            mo.lpstrAlias       = "ding_midi";
            mo.lpstrDeviceType  = "mpegvideo";
            if (mciSendCommand(0, MCI_OPEN,
                    MCI_OPEN_ELEMENT | MCI_OPEN_ALIAS | MCI_OPEN_TYPE,
                    (DWORD)(LPVOID)&mo) == 0)
                gm->mci_ding_id = mo.wDeviceID;
        }
    } else {
        /* Windows: midiOut works fine */
        if (midiOutOpen(&gm->midi_out, MIDI_MAPPER, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR) {
            /* CC7 (channel volume) + CC11 (expression) on ch10 to max */
            midiOutShortMsg(gm->midi_out, 0x007F07B9);
            midiOutShortMsg(gm->midi_out, 0x007F0BB9);
        }
    }

    return 0;
}

static void rg_cleanup(RhythmGame *gm) {
    if (gm->mci_device_id) {
        mciSendCommand(gm->mci_device_id, MCI_STOP,  0, 0);
        mciSendCommand(gm->mci_device_id, MCI_CLOSE, 0, 0);
        gm->mci_device_id = 0;
    }
    if (gm->mci_ding_id) {
        mciSendCommand(gm->mci_ding_id, MCI_STOP,  0, 0);
        mciSendCommand(gm->mci_ding_id, MCI_CLOSE, 0, 0);
        gm->mci_ding_id = 0;
        if (gm->ding_tmp[0]) DeleteFileA(gm->ding_tmp);
    }
    if (gm->midi_out) {
        if (gm->note_off_at)
            midiOutShortMsg(gm->midi_out, 0x000078B9); /* CC120 (All Sound Off) ch10 */
        midiOutClose(gm->midi_out);
        gm->midi_out = NULL;
    }
    free(gm->onset_times);
    free(gm->track_indices);
    free(gm->hit_status);
    free(gm->bg_pixels);
}

/* ── render ──────────────────────────────────────────────────────────────── */
static void rg_render(RhythmGame *gm) {
    /* Background */
    if (gm->bg_pixels)
        memcpy(g_videoram, gm->bg_pixels, IMAGE_AREA_PIXELS * sizeof(uint32_t));
    else
        for (int i = 0; i < IMAGE_AREA_PIXELS; i++)
            g_videoram[i] = COLOR_BLACK;

    /* Countdown: large white digit centred on screen, with 1px black outline */
    if (gm->countdown > 0) {
        char d;
        int scale = 6;
        int cw = 8 * scale, ch = 15 * scale;
        d = (char)('0' + gm->countdown);
        rg_putc_big_outlined((SCREEN_WIDTH - cw) / 2, (TEXT_AREA_START - ch) / 2,
                             d, scale, COLOR_WHITE);
        return;
    }

    /* White track lines */
    for (int i = 0; i < NUM_TRACKS; i++) {
        int x = X_OFFSET + i * TRACK_SPACING;
        rg_rect(x - TRACK_WIDTH/2, 0, TRACK_WIDTH, TEXT_AREA_START, COLOR_WHITE);
    }

    /* Blue hit-zone bar spanning all tracks, fills with dark blue as song progresses */
    int bar_x1 = X_OFFSET - TRACK_WIDTH/2;
    int bar_x2 = X_OFFSET + (NUM_TRACKS - 1) * TRACK_SPACING + TRACK_WIDTH/2;
    int bar_w = bar_x2 - bar_x1;
    float music_time = (float)(int)(timeGetTime() - gm->music_start_time) / 1000.0f;
    float song_duration = (gm->num_notes > 0) ? gm->onset_times[gm->num_notes - 1] : 1.0f;
    float progress = music_time / song_duration;
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;
    int filled_w = (int)(progress * bar_w);
    if (filled_w > 0)    rg_rect(bar_x1, BAR_Y, filled_w, BAR_HEIGHT, COLOR_DKBLUE);
    if (filled_w < bar_w) rg_rect(bar_x1 + filled_w, BAR_Y, bar_w - filled_w, BAR_HEIGHT, COLOR_BLUE);

    /* Notes: fall from top toward the bar at bottom */
    float note_offset_s = NOTE_OFFSET_MS / 1000.0f;

    for (int i = 0; i < gm->num_notes; i++) {
        if (gm->hit_status[i] != 0) continue;
        float td = gm->onset_times[i] - music_time; /* >0 = future, 0 = hit zone */
        if (td < 0.0f || td > note_offset_s) continue;
        float frac = td / note_offset_s;             /* 1=just appeared at top, 0=at bar */
        int y_center = BAR_MID_Y - (int)(frac * (BAR_MID_Y + NOTE_RADIUS));
        int is_large = (rg_fabsf(td) < HIT_THRESHOLD / 1000.0f) ? 1 : 0;
        rg_draw_note(gm->track_indices[i], y_center, is_large);
    }

    /* UI text: top-left, above the falling notes */
    char score_buf[64];
    snprintf(score_buf, sizeof score_buf, "Score: %d", gm->score);
    rg_puts_outlined(10, 5, score_buf, COLOR_WHITE);
    if (gm->combo > 1) {
        char combo_buf[32];
        snprintf(combo_buf, sizeof combo_buf, "Combo x%d (+%d%%)", gm->combo, gm->combo * 10);
        rg_puts_outlined(10, 22, combo_buf, COLOR_ORANGE);
    }
}

/* ── main loop ───────────────────────────────────────────────────────────── */
int PlayRhythmGame(const char *bg_path, const char *audio_path, const char *beatmap_path, int stride) {
    RhythmGame gm;
    MSG msg;
    int rollback = 0, quit = 0;

    if (rg_init(bg_path, audio_path, beatmap_path, stride, &gm) != 0)
        return -3;

    g_effectrunning = 1;
    g_lastkey = 0;

    while (!gm.has_ended && g_running && !quit) {

        /* Tick countdown */
        if (gm.countdown > 0) {
            DWORD elapsed = timeGetTime() - gm.countdown_start;
            if (elapsed >= 1000) {
                gm.countdown--;
                gm.countdown_start = timeGetTime();
                if (gm.countdown == 0)
                    rg_start_music(&gm);   /* start audio when 0 is reached */
            }
        }

        /* Process window messages */
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = 0;
                break;
            }
            if (msg.message == WM_KEYDOWN) {
                switch (msg.wParam) {
                    case VK_LEFT: case 'A':
                    case VK_UP: case 'S':
                    case VK_DOWN: case VK_NUMPAD1:
                    case VK_RIGHT: case VK_NUMPAD2:
                        if (gm.countdown == 0 && !gm.has_ended) {
                            int track = (msg.wParam == VK_LEFT  || msg.wParam == 'A')          ? 0 :
                                        (msg.wParam == VK_UP    || msg.wParam == 'S')          ? 1 :
                                        (msg.wParam == VK_DOWN  || msg.wParam == VK_NUMPAD1)   ? 2 :
                                        /* VK_RIGHT, VK_NUMPAD2 */                               3;
                            float mt = (float)(int)(timeGetTime() - gm.music_start_time) / 1000.0f;
                            for (int i = 0; i < gm.num_notes; i++) {
                                if (gm.hit_status[i] == 0 && gm.track_indices[i] == track) {
                                    if (rg_fabsf(gm.onset_times[i] - mt) < HIT_THRESHOLD/1000.0f) {
                                        gm.hit_status[i] = 1;
                                        gm.num_hits++;
                                        gm.score += 10 + gm.combo;
                                        gm.combo++;
                                        if (IsWine()) {
                                            if (gm.mci_ding_id) {
                                                MCI_SEEK_PARMS sp; memset(&sp, 0, sizeof sp);
                                                mciSendCommand(gm.mci_ding_id, MCI_SEEK, MCI_SEEK_TO_START, (DWORD)(LPVOID)&sp);
                                                MCI_PLAY_PARMS pp; memset(&pp, 0, sizeof pp);
                                                mciSendCommand(gm.mci_ding_id, MCI_PLAY, 0, (DWORD)(LPVOID)&pp);
                                            }
                                        } else if (gm.midi_out) {
                                            /* GM ch10 (0x99) note 49=0x31 (Crash Cymbal 1), vel 127 */
                                            midiOutShortMsg(gm.midi_out, 0x007F3199);
                                            gm.note_off_at = timeGetTime() + 500;
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                        break;
                    case 'B':
                        rollback = 1; gm.has_ended = 1; break;
                    case 'Q': {
                        /* Wine MCI_PAUSE/MCI_RESUME is broken */
                        if(IsWine()) break;

                        DWORD pause_pos = 0;
                        DWORD pause_start = timeGetTime();
                        /* Pause music and capture position while stable */
                        MCI_GENERIC_PARMS gp;
                        memset(&gp, 0, sizeof gp);
                        if (gm.mci_playing && !gm.mci_play_at) {
                            mciSendCommand(gm.mci_device_id, MCI_PAUSE, 0, (DWORD)(LPVOID)&gp);
                            MCI_STATUS_PARMS sp;
                            memset(&sp, 0, sizeof sp);
                            sp.dwItem = MCI_STATUS_POSITION;
                            mciSendCommand(gm.mci_device_id, MCI_STATUS, MCI_STATUS_ITEM, (DWORD)(LPVOID)&sp);
                            pause_pos = sp.dwReturn;
                        }
                        /* Show quit dialog (use temp buffer to avoid corrupting g_background) */
                        g_effectrunning = 0;
                        uint32_t *qsave = (uint32_t *)malloc(IMAGE_AREA_PIXELS * sizeof(uint32_t));
                        if (qsave) memcpy(qsave, g_videoram, IMAGE_AREA_PIXELS * sizeof(uint32_t));
                        DispQuit();
                        int qn = read_keyboard_status();
                        while ((qn != 9 && qn != 10 && qn != 11) && g_running) {
                            if (qn == 7) RestoreWindowSize();
                            qn = read_keyboard_status();
                            Sleep(5);
                        }
                        if (qn == 10) {
                            quit = 1; gm.has_ended = 1;
                        }
                        if (qsave) { memcpy(g_videoram, qsave, IMAGE_AREA_PIXELS * sizeof(uint32_t)); free(qsave); }
                        update_display();
                        g_effectrunning = 1;
                        if (!quit) {
                            DWORD paused = timeGetTime() - pause_start;
                            if (gm.countdown > 0)
                                gm.countdown_start += paused;
                            if (gm.mci_play_at) {
                                gm.mci_play_at += paused;
                                gm.music_start_time += paused;
                            } else if (gm.mci_playing) {
                                mciSendCommand(gm.mci_device_id, MCI_RESUME, 0, (DWORD)(LPVOID)&gp);
                                /* Re-anchor timeline from the stable paused position */
                                DWORD now = timeGetTime();
                                gm.music_start_time = (now >= pause_pos) ? (now - pause_pos) : 0;
                            }
                        }
                        break;
                    }
                    case 'R':
                        RestoreWindowSize(); break;
                }
            } else {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }

        /* Deferred MCI start (after NOTE_OFFSET_MS delay) */
        if (gm.mci_play_at && timeGetTime() >= gm.mci_play_at) {
            MCI_PLAY_PARMS p;
            memset(&p, 0, sizeof p);
            p.dwCallback = (DWORD)g_hwnd;
            mciSendCommand(gm.mci_device_id, MCI_PLAY, MCI_NOTIFY, (DWORD)(LPVOID)&p);
            gm.music_start_time = timeGetTime(); /* resync to actual MCI start */
            gm.mci_play_at = 0;
        }

        /* End when music finishes */
        if (gm.countdown == 0 && !gm.mci_play_at && !gm.has_ended) {
            if (!rg_music_playing(gm.mci_device_id)) {
                Sleep(1500);
                gm.has_ended = 1;
            }
        }

        /* Send note-off when due */
        if (gm.note_off_at && timeGetTime() >= gm.note_off_at) {
            midiOutShortMsg(gm.midi_out, 0x000078B9); /* CC120 (All Sound Off) ch10 */
            gm.note_off_at = 0;
        }

        /* Detect missed notes (passed hit window without being hit) */
        if (gm.countdown == 0 && !gm.mci_play_at) {
            float mt = (float)(int)(timeGetTime() - gm.music_start_time) / 1000.0f;
            for (int i = 0; i < gm.num_notes; i++) {
                if (gm.hit_status[i] == 0 && mt > gm.onset_times[i] + HIT_THRESHOLD / 1000.0f) {
                    gm.hit_status[i] = -1; /* mark as missed */
                    gm.combo = 0;
                }
            }
        }

        rg_render(&gm);
        update_display();
        Sleep(16); /* ~60 fps */
    }

    int score = quit ? -2 : rollback ? -1 : gm.score;

    /* Check for full combo (no notes missed) */
    if (score >= 0) {
        int missed = 0;
        for (int i = 0; i < gm.num_notes; i++)
            if (gm.hit_status[i] == -1) { missed = 1; break; }
        if (!missed) g_fullcombo = 1;
    }

    rg_cleanup(&gm);
    g_effectrunning = 0;
    g_lastkey = 0;

    /* Update high score if score improved */
    if (score >= 0) {
        /* Extract bare filename from audio_path (e.g. "data\foo.wav" -> "foo.wav") */
        const char *fname = audio_path;
        const char *p = audio_path;
        while (*p) { if (*p == '\\' || *p == '/') fname = p + 1; p++; }

        FILE *fp = fopen(RGSCORE_FILE, "r");
        if (fp) {
            char lines[RGSCORE_MAX][512];
            int  nlines = 0;
            int  updated = 0;
            while (nlines < RGSCORE_MAX && fgets(lines[nlines], 512, fp)) {
                int len = (int)strlen(lines[nlines]);
                while (len > 0 && (lines[nlines][len-1] == '\n' || lines[nlines][len-1] == '\r'))
                    lines[nlines][--len] = '\0';
                nlines++;
            }
            fclose(fp);

            int flen = (int)strlen(fname);
            for (int i = 0; i < nlines; i++) {

                if (strncmp(lines[i], fname, flen) == 0 && lines[i][flen] == '|') {
                    const char *sep2 = strchr(lines[i] + flen + 1, '|');
                    int old_score = sep2 ? atoi(sep2 + 1) : 0;
                    if (score > old_score) {
                        char name_field[256] = {0};
                        if (sep2) {
                            int nlen = (int)(sep2 - (lines[i] + flen + 1));
                            if (nlen >= 256) nlen = 255;
                            memcpy(name_field, lines[i] + flen + 1, nlen);
                        }
                        snprintf(lines[i], 512, "%s|%s|%d", fname, name_field, score);
                        updated = 1;
                    }
                    break;
                }
            }

            if (updated) {
                fp = fopen(RGSCORE_FILE, "w");
                if (fp) {
                    for (int j = 0; j < nlines; j++)
                        if (lines[j][0] != '\0')
                            fprintf(fp, "%s\n", lines[j]);
                    fclose(fp);
                }
            }
        }
    }

    return score;
}
