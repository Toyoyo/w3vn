/*
 *      Rhythm Game High Score Screen for STVN Engine - Win32s Port
 *      (c) 2026 Toyoyo
 *
 *      Loads a background image, reads data\rgscore.txt (filename|name|score),
 *      and displays "name: score" entries with outlined text in the image area.
 *      Returns -1 on 'B' key, -3 on Space.
 */

/* Included into w3vn.c after func.c and rythm.c */

typedef struct {
    char name[256];
    int  score;
} RgScoreEntry;

static int rgs_load(RgScoreEntry *entries, int max_entries) {
    FILE *fp = fopen(RGSCORE_FILE, "r");
    if (!fp) return 0;
    int count = 0;
    char line[512];
    while (count < max_entries && fgets(line, sizeof(line), fp)) {
        /* Strip trailing newline */
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;

        /* Format: filename|name|score */
        const char *sep1 = strchr(line, '|');
        if (!sep1) continue;
        const char *sep2 = strchr(sep1 + 1, '|');
        if (!sep2) continue;

        int name_len = (int)(sep2 - (sep1 + 1));
        if (name_len <= 0 || name_len >= 256) continue;
        memcpy(entries[count].name, sep1 + 1, name_len);
        entries[count].name[name_len] = '\0';
        entries[count].score = atoi(sep2 + 1);
        count++;
    }
    fclose(fp);
    return count;
}

int ShowRgScore(const char *img_path) {
    uint8_t temp_palette[768];

    /* Load background into image area of g_videoram */
    if (LoadBackgroundImage(img_path, temp_palette, g_videoram) != 0) {
        /* Fallback: black background */
        memset(g_videoram, 0, IMAGE_AREA_PIXELS * sizeof(uint32_t));
    }

    /* Load scores */
    RgScoreEntry entries[RGSCORE_MAX];
    int count = rgs_load(entries, RGSCORE_MAX);

    /* Print each entry: "name: score" with 1px black outline */
    int x = 16;
    int y = 16;
    for (int i = 0; i < count; i++) {
        char buf[280];
        snprintf(buf, sizeof(buf), "%s: %d", entries[i].name, entries[i].score);
        rg_puts_outlined(x, y, buf, COLOR_WHITE);
        y += 18;
        if (y + 15 >= TEXT_AREA_START) break;
    }

    update_display();

    /* Wait for Space (return -3) or B (return -1) */
    g_lastkey = 0;
    while (g_running) {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        int k = g_lastkey;
        if (k == 1) { g_lastkey = 0; return -3; } /* Space */
        if (k == 5) { g_lastkey = 0; return -1; } /* B */
        Sleep(5);
    }
    return -3;
}
