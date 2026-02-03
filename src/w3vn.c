/*
 *      STVN Engine - Win32s Port
 *      (c) 2022, 2023, 2026 Toyoyo
 *      Win32s port 2026
 *
 *      This library is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU Lesser General Public
 *      License as published by the Free Software Foundation; either
 *      version 2.1 of the License, or (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *      Lesser General Public License for more details.
 *
 *      You should have received a copy of the GNU Lesser General Public
 *      License along with this library; if not, write to the Free Software
 *      Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <io.h>
#include <fcntl.h>
#include <zlib.h>
#include <png.h>

/* Link with winmm.lib for MCI audio */
#pragma comment(lib, "winmm.lib")

/* Screen dimensions - Atari ST hi-res, now 32-bit color */
#define SCREEN_WIDTH  640
#define SCREEN_HEIGHT 400

/* Text area starts at line 320 */
#define TEXT_AREA_START 320

/* Colors in BGRA format (Win32 32-bit DIB) */
#define COLOR_WHITE 0xFFFFFFFF
#define COLOR_BLACK 0xFF000000

/* Used to check a valid choice in Loading/Saving dialogs */
#define NoValidSaveChoice(n) ((n) != 2 && ((n) < 10 || (n) > 19))

/* Global window and rendering state */
static HWND g_hwnd = NULL;
static HICON g_hIcon = NULL;
static uint32_t *g_videoram = NULL;     /* Our virtual framebuffer (32-bit BGRA) */
static char g_windowTitle[128] = "STVN Engine - Win32s";  /* Default window title */
static uint32_t *g_background = NULL;   /* Background image buffer (32-bit BGRA) */
static uint32_t *g_textarea = NULL;     /* Text area buffer (32-bit BGRA) */
static volatile int g_running = 1;
static volatile int g_lastkey = 0;
static volatile int g_mouseclick = 0;
static volatile int g_windowactive = 1;
static volatile int g_ignoreclick = 0;  /* Ignore click that activated window */
static volatile int g_titlebarclick = 0;  /* Activation came from title bar */
static volatile int g_effectrunning = 0;  /* Effect is currently rendering */

/* Character rendering state */
static int g_cursorX = 0;
static int g_cursorY = 0;

/* Audio state - using MCI for Win32s compatibility */
static UINT g_mciDeviceID = 0;
static char g_currentMusic[260] = {0};
#define MUSIC_TIMER_ID 1

/* Video state - using MCI for AVI playback */
static int g_videoPlaying = 0;

/* Forward declaration */
static void RestartMusic(void);
static void CheckMusicStatus(void);

/* Start playing a music file (WAV or MID) */
static void PlayMusic(const char *filename) {
    MCI_OPEN_PARMS mciOpen;
    MCI_PLAY_PARMS mciPlay;
    DWORD dwReturn;
    char fullpath[260];

    /* Stop any currently playing music */
    if (g_mciDeviceID != 0) {
        mciSendCommand(g_mciDeviceID, MCI_STOP, 0, 0);
        mciSendCommand(g_mciDeviceID, MCI_CLOSE, 0, 0);
        g_mciDeviceID = 0;
    }

    /* Get full path to the file */
    if (GetFullPathNameA(filename, 260, fullpath, NULL) == 0) {
        strncpy(fullpath, filename, 259);
        fullpath[259] = '\0';
    }

    /* Check if file exists */
    if (GetFileAttributesA(fullpath) == INVALID_FILE_ATTRIBUTES) {
        return;
    }

    /* Open the audio file - let MCI auto-detect the type */
    memset(&mciOpen, 0, sizeof(mciOpen));
    mciOpen.lpstrElementName = fullpath;

    dwReturn = mciSendCommand(0, MCI_OPEN, MCI_OPEN_ELEMENT, (DWORD)(LPVOID)&mciOpen);
    if (dwReturn != 0) {
        return;
    }

    g_mciDeviceID = mciOpen.wDeviceID;
    strncpy(g_currentMusic, filename, 259);
    g_currentMusic[259] = '\0';

    /* Start playback with notification for looping */
    memset(&mciPlay, 0, sizeof(mciPlay));
    mciPlay.dwCallback = (DWORD)g_hwnd;
    mciSendCommand(g_mciDeviceID, MCI_PLAY, MCI_NOTIFY, (DWORD)(LPVOID)&mciPlay);

    /* Start timer to poll playback status (for Win32s compatibility) */
    SetTimer(g_hwnd, MUSIC_TIMER_ID, 500, NULL);
}

/* Restart music from beginning (for looping) */
static void RestartMusic(void) {
    if (g_mciDeviceID == 0 || g_currentMusic[0] == '\0') return;

    /* Close and reopen for ADPCM compatibility */
    mciSendCommand(g_mciDeviceID, MCI_STOP, 0, 0);
    mciSendCommand(g_mciDeviceID, MCI_CLOSE, 0, 0);
    g_mciDeviceID = 0;
    PlayMusic(g_currentMusic);
}

/* Check if music has stopped and restart if needed (for Win32s compatibility) */
static void CheckMusicStatus(void) {
    MCI_STATUS_PARMS mciStatus;

    if (g_mciDeviceID == 0 || g_currentMusic[0] == '\0') return;

    memset(&mciStatus, 0, sizeof(mciStatus));
    mciStatus.dwItem = MCI_STATUS_MODE;
    if (mciSendCommand(g_mciDeviceID, MCI_STATUS, MCI_STATUS_ITEM, (DWORD)(LPVOID)&mciStatus) == 0) {
        if (mciStatus.dwReturn == MCI_MODE_STOP) {
            RestartMusic();
        }
    }
}

/* Stop currently playing music */
static void StopMusic(void) {
    if (g_mciDeviceID != 0) {
        KillTimer(g_hwnd, MUSIC_TIMER_ID);
        mciSendCommand(g_mciDeviceID, MCI_STOP, 0, 0);
        mciSendCommand(g_mciDeviceID, MCI_CLOSE, 0, 0);
        g_mciDeviceID = 0;
        g_currentMusic[0] = '\0';
    }
}

/* Play a video file (AVI) in the image area */
static void PlayVideo(const char *filename) {
    char cmd[512];
    RECT rect;
    int video_h;

    /* Stop any currently playing video */
    if (g_videoPlaying) {
        mciSendString("stop video", NULL, 0, NULL);
        mciSendString("close video", NULL, 0, NULL);
        g_videoPlaying = 0;
    }

    /* Open the video file */
    snprintf(cmd, sizeof(cmd), "open \"%s\" alias video", filename);
    if (mciSendString(cmd, NULL, 0, NULL) != 0) {
        return;
    }

    /* Tell MCI to use our window for video display */
    snprintf(cmd, sizeof(cmd), "window video handle %lu", (unsigned long)(uintptr_t)g_hwnd);
    mciSendString(cmd, NULL, 0, NULL);

    /* Set destination rectangle (image area) */
    GetClientRect(g_hwnd, &rect);
    video_h = (rect.bottom * TEXT_AREA_START) / SCREEN_HEIGHT;
    snprintf(cmd, sizeof(cmd), "put video destination at 0 0 %d %d", (int)rect.right, video_h);
    mciSendString(cmd, NULL, 0, NULL);

    /* Start playback */
    mciSendString("play video", NULL, 0, NULL);
    g_videoPlaying = 1;

    /* Restore focus to our window */
    SetFocus(g_hwnd);
}

/* Stop currently playing video */
static void StopVideo(void) {
    /* Always try to stop/close, even if g_videoPlaying is 0 (state may be out of sync) */
    mciSendString("stop video wait", NULL, 0, NULL);
    mciSendString("close video wait", NULL, 0, NULL);
    g_videoPlaying = 0;
}

/* Check if video is still playing */
static int IsVideoPlaying(void) {
    char status[64] = {0};
    if (!g_videoPlaying) return 0;
    if (mciSendString("status video mode", status, sizeof(status), NULL) == 0) {
        if (strcmp(status, "playing") == 0) return 1;
    }
    return 0;
}

/* 8x15 VGA-style font covering ASCII 32-127 and CP1252 128-255 */
static const uint8_t g_font8x15[224][15] = {
    /* ASCII 32-127 (96 characters) */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 32 Space */
    {0x00,0x00,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00}, /* 33 ! */
    {0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 34 " */
    {0x00,0x00,0x00,0x6C,0x6C,0xFE,0x6C,0x6C,0x6C,0xFE,0x6C,0x6C,0x00,0x00,0x00}, /* 35 # */
    {0x00,0x18,0x18,0x7C,0xC6,0xC2,0xC0,0x7C,0x06,0x86,0xC6,0x7C,0x18,0x18,0x00}, /* 36 $ */
    {0x00,0x00,0x00,0x00,0xC2,0xC6,0x0C,0x18,0x30,0x60,0xC6,0x86,0x00,0x00,0x00}, /* 37 % */
    {0x00,0x00,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00}, /* 38 & */
    {0x00,0x30,0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 39 ' */
    {0x00,0x00,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0x00,0x00,0x00}, /* 40 ( */
    {0x00,0x00,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x00,0x00,0x00}, /* 41 ) */
    {0x00,0x00,0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00}, /* 42 * */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,0x00,0x00,0x00}, /* 43 + */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00}, /* 44 , */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 45 - */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00}, /* 46 . */
    {0x00,0x00,0x00,0x00,0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00,0x00,0x00}, /* 47 / */
    {0x00,0x00,0x7C,0xC6,0xC6,0xCE,0xDE,0xF6,0xE6,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 48 0 */
    {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00}, /* 49 1 */
    {0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC6,0xFE,0x00,0x00,0x00}, /* 50 2 */
    {0x00,0x00,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00}, /* 51 3 */
    {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x1E,0x00,0x00,0x00}, /* 52 4 */
    {0x00,0x00,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00}, /* 53 5 */
    {0x00,0x00,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 54 6 */
    {0x00,0x00,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x00,0x00,0x00}, /* 55 7 */
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 56 8 */
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x06,0x0C,0x78,0x00,0x00,0x00}, /* 57 9 */
    {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00}, /* 58 : */
    {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00}, /* 59 ; */
    {0x00,0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00}, /* 60 < */
    {0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00}, /* 61 = */
    {0x00,0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00}, /* 62 > */
    {0x00,0x00,0x7C,0xC6,0xC6,0x0C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00}, /* 63 ? */
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xDE,0xDE,0xDE,0xDC,0xC0,0x7C,0x00,0x00,0x00}, /* 64 @ */
    {0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00}, /* 65 A */
    {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0xFC,0x00,0x00,0x00}, /* 66 B */
    {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0x00,0x00,0x00}, /* 67 C */
    {0x00,0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00}, /* 68 D */
    {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x62,0x66,0xFE,0x00,0x00,0x00}, /* 69 E */
    {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0x60,0xF0,0x00,0x00,0x00}, /* 70 F */
    {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0x00,0x00,0x00}, /* 71 G */
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00}, /* 72 H */
    {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00}, /* 73 I */
    {0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0xCC,0x78,0x00,0x00,0x00}, /* 74 J */
    {0x00,0x00,0xE6,0x66,0x66,0x6C,0x78,0x78,0x6C,0x66,0x66,0xE6,0x00,0x00,0x00}, /* 75 K */
    {0x00,0x00,0xF0,0x60,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xFE,0x00,0x00,0x00}, /* 76 L */
    {0x00,0x00,0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00}, /* 77 M */
    {0x00,0x00,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00}, /* 78 N */
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 79 O */
    {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00}, /* 80 P */
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0C,0x0E,0x00}, /* 81 Q */
    {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0x66,0xE6,0x00,0x00,0x00}, /* 82 R */
    {0x00,0x00,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0x06,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 83 S */
    {0x00,0x00,0xFF,0xDB,0x99,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00}, /* 84 T */
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 85 U */
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00,0x00,0x00}, /* 86 V */
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xD6,0xFE,0x6C,0x6C,0x00,0x00,0x00}, /* 87 W */
    {0x00,0x00,0xC6,0xC6,0x6C,0x6C,0x38,0x38,0x6C,0x6C,0xC6,0xC6,0x00,0x00,0x00}, /* 88 X */
    {0x00,0x00,0xC3,0xC3,0x66,0x66,0x3C,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00}, /* 89 Y */
    {0x00,0x00,0xFE,0xC6,0x86,0x0C,0x18,0x30,0x60,0xC2,0xC6,0xFE,0x00,0x00,0x00}, /* 90 Z */
    {0x00,0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00,0x00}, /* 91 [ */
    {0x00,0x00,0x00,0x80,0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00,0x00,0x00,0x00}, /* 92 \ */
    {0x00,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00,0x00}, /* 93 ] */
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 94 ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00}, /* 95 _ */
    {0x30,0x30,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 96 ` */
    {0x00,0x00,0x00,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00}, /* 97 a */
    {0x00,0x00,0xE0,0x60,0x60,0x78,0x6C,0x66,0x66,0x66,0x66,0xDC,0x00,0x00,0x00}, /* 98 b */
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00}, /* 99 c */
    {0x00,0x00,0x1C,0x0C,0x0C,0x3C,0x6C,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00}, /* 100 d */
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xFE,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00}, /* 101 e */
    {0x00,0x00,0x38,0x6C,0x64,0x60,0xF0,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00}, /* 102 f */
    {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78}, /* 103 g */
    {0x00,0x00,0xE0,0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0x66,0xE6,0x00,0x00,0x00}, /* 104 h */
    {0x00,0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00}, /* 105 i */
    {0x00,0x00,0x06,0x06,0x00,0x0E,0x06,0x06,0x06,0x06,0x06,0x06,0x66,0x66,0x3C}, /* 106 j */
    {0x00,0x00,0xE0,0x60,0x60,0x66,0x6C,0x78,0x78,0x6C,0x66,0xE6,0x00,0x00,0x00}, /* 107 k */
    {0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00}, /* 108 l */
    {0x00,0x00,0x00,0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0xD6,0xD6,0x00,0x00,0x00}, /* 109 m */
    {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00}, /* 110 n */
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 111 o */
    {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0}, /* 112 p */
    {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x1E}, /* 113 q */
    {0x00,0x00,0x00,0x00,0x00,0xDC,0x76,0x62,0x60,0x60,0x60,0xF0,0x00,0x00,0x00}, /* 114 r */
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00,0x00,0x00}, /* 115 s */
    {0x00,0x00,0x10,0x30,0x30,0xFC,0x30,0x30,0x30,0x30,0x36,0x1C,0x00,0x00,0x00}, /* 116 t */
    {0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00}, /* 117 u */
    {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00,0x00,0x00}, /* 118 v */
    {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0x6C,0x00,0x00,0x00}, /* 119 w */
    {0x00,0x00,0x00,0x00,0x00,0xC6,0x6C,0x38,0x38,0x38,0x6C,0xC6,0x00,0x00,0x00}, /* 120 x */
    {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0xF8}, /* 121 y */
    {0x00,0x00,0x00,0x00,0x00,0xFE,0xCC,0x18,0x30,0x60,0xC6,0xFE,0x00,0x00,0x00}, /* 122 z */
    {0x00,0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x18,0x0E,0x00,0x00,0x00}, /* 123 { */
    {0x00,0x00,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00}, /* 124 | */
    {0x00,0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x18,0x70,0x00,0x00,0x00}, /* 125 } */
    {0x00,0x00,0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 126 ~ */
    {0x00,0x00,0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xC6,0xFE,0x00,0x00,0x00,0x00}, /* 127 DEL */
    /* CP1252 128-159 (32 characters) */
    {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xF8,0xC0,0xC2,0x66,0x3C,0x00,0x00,0x00,0x00}, /* 128 Euro */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 129 undef */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00}, /* 130 sgl low q */
    {0x00,0x00,0x1C,0x36,0x32,0x30,0x78,0x30,0x30,0x30,0x30,0x78,0x00,0x00,0x00}, /* 131 f hook */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x6C,0x6C,0x24,0x48,0x00,0x00}, /* 132 dbl low q */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x36,0x36,0x36,0x00,0x00,0x00,0x00}, /* 133 ellipsis */
    {0x00,0x00,0x18,0x3C,0x3C,0x3C,0xFF,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00}, /* 134 dagger */
    {0x00,0x00,0x18,0x3C,0x3C,0xFF,0x18,0xFF,0x18,0x18,0x18,0x18,0x00,0x00,0x00}, /* 135 dbl dag */
    {0x10,0x38,0x6C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 136 circum */
    {0x00,0x00,0x00,0x00,0xC4,0xC6,0x6C,0x10,0x28,0xC6,0x86,0x00,0x00,0x00,0x00}, /* 137 permil */
    {0x00,0x60,0x30,0x00,0x7C,0xC6,0x60,0x38,0x0C,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 138 S caron */
    {0x00,0x00,0x00,0x00,0x00,0x36,0x6C,0xD8,0x6C,0x36,0x00,0x00,0x00,0x00,0x00}, /* 139 sgl L ang */
    {0x00,0x00,0x00,0x00,0x7E,0xDB,0xDB,0xDF,0xD8,0xDB,0xDB,0x7E,0x00,0x00,0x00}, /* 140 OE */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 141 undef */
    {0x00,0x60,0x30,0x00,0xFE,0xC6,0x86,0x0C,0x18,0x30,0xC2,0xFE,0x00,0x00,0x00}, /* 142 Z caron */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 143 undef */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 144 undef */
    {0x00,0x60,0x60,0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 145 L sgl q */
    {0x00,0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 146 R sgl q */
    {0x00,0x6C,0x6C,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 147 L dbl q */
    {0x00,0x48,0x6C,0x6C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 148 R dbl q */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x3C,0x3C,0x18,0x00,0x00,0x00,0x00,0x00,0x00}, /* 149 bullet */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 150 en dash */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 151 em dash */
    {0x00,0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 152 sm tilde */
    {0x00,0x00,0x00,0x7E,0xDB,0xDF,0xD8,0xD8,0xDB,0x7E,0x00,0x00,0x00,0x00,0x00}, /* 153 TM */
    {0x00,0x60,0x30,0x00,0x7C,0xC6,0x60,0x38,0x0C,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 154 s caron */
    {0x00,0x00,0x00,0x00,0x00,0xD8,0x6C,0x36,0x6C,0xD8,0x00,0x00,0x00,0x00,0x00}, /* 155 sgl R ang */
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xD6,0xDE,0xD0,0xD0,0xD6,0x7C,0x00,0x00,0x00}, /* 156 oe */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 157 undef */
    {0x00,0x60,0x30,0x00,0x00,0xFE,0xCC,0x18,0x30,0x60,0xC6,0xFE,0x00,0x00,0x00}, /* 158 z caron */
    {0x00,0xC6,0x00,0xC3,0xC3,0x66,0x66,0x3C,0x18,0x18,0x18,0x3C,0x00,0x00,0x00}, /* 159 Y diaer */
    /* CP1252 160-191 (32 characters) */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 160 NBSP */
    {0x00,0x00,0x18,0x18,0x00,0x18,0x18,0x18,0x3C,0x3C,0x3C,0x18,0x00,0x00,0x00}, /* 161 inv ! */
    {0x00,0x18,0x18,0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x18,0x18,0x00,0x00,0x00}, /* 162 cent */
    {0x00,0x00,0x38,0x6C,0x64,0x60,0xF0,0x60,0x60,0x66,0xE6,0xFC,0x00,0x00,0x00}, /* 163 pound */
    {0x00,0x00,0x00,0x00,0xC6,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0x00,0x00,0x00,0x00}, /* 164 currency */
    {0x00,0x00,0xC3,0x66,0x3C,0x18,0xFF,0x18,0xFF,0x18,0x18,0x18,0x00,0x00,0x00}, /* 165 yen */
    {0x00,0x00,0x18,0x18,0x18,0x18,0x00,0x00,0x18,0x18,0x18,0x18,0x00,0x00,0x00}, /* 166 brk bar */
    {0x00,0x7C,0xC6,0x60,0x38,0x6C,0xC6,0xC6,0x6C,0x38,0x0C,0xC6,0x7C,0x00,0x00}, /* 167 section */
    {0x00,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 168 diaer */
    {0x00,0x00,0x7C,0xC6,0xC6,0xDE,0xD6,0xDE,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, /* 169 (C) */
    {0x00,0x3C,0x0C,0x3C,0x6C,0x3C,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 170 fem ord */
    {0x00,0x00,0x00,0x00,0x00,0x36,0x6C,0xD8,0x6C,0x36,0x00,0x00,0x00,0x00,0x00}, /* 171 L guill */
    {0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x06,0x06,0x06,0x06,0x00,0x00,0x00,0x00}, /* 172 not */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 173 SHY */
    {0x00,0x00,0x7C,0xC6,0xC6,0xF6,0xDE,0xF6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, /* 174 (R) */
    {0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 175 macron */
    {0x00,0x38,0x6C,0x6C,0x38,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 176 degree */
    {0x00,0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x7E,0x00,0x00,0x00,0x00}, /* 177 +/- */
    {0x00,0x38,0x6C,0x0C,0x18,0x30,0x7C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 178 sup 2 */
    {0x00,0x38,0x6C,0x0C,0x38,0x0C,0x6C,0x38,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 179 sup 3 */
    {0x00,0x0C,0x18,0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 180 acute */
    {0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xFE,0xC0,0xC0,0x00}, /* 181 micro */
    {0x00,0x00,0x7F,0xDB,0xDB,0xDB,0x7B,0x1B,0x1B,0x1B,0x1B,0x1B,0x00,0x00,0x00}, /* 182 pilcrow */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 183 mid dot */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x0C,0x38}, /* 184 cedilla */
    {0x00,0x30,0x70,0x30,0x30,0x30,0x78,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 185 sup 1 */
    {0x00,0x38,0x6C,0x6C,0x38,0x00,0x7C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 186 masc ord */
    {0x00,0x00,0x00,0x00,0x00,0xD8,0x6C,0x36,0x6C,0xD8,0x00,0x00,0x00,0x00,0x00}, /* 187 R guill */
    {0x00,0xC0,0xC0,0xC2,0xC6,0xCC,0x18,0x30,0x60,0xCE,0x9B,0x06,0x0C,0x0F,0x00}, /* 188 1/4 */
    {0x00,0xC0,0xC0,0xC2,0xC6,0xCC,0x18,0x30,0x66,0xCE,0x96,0x3E,0x06,0x06,0x00}, /* 189 1/2 */
    {0x00,0xE0,0x30,0x62,0x36,0xEC,0x18,0x30,0x60,0xCE,0x9B,0x06,0x0C,0x0F,0x00}, /* 190 3/4 */
    {0x00,0x00,0x30,0x30,0x00,0x30,0x30,0x60,0xC0,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 191 inv ? */
    /* CP1252 192-223: uppercase accented (32 characters) */
    {0x60,0x30,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00,0x00,0x00}, /* 192 A grav */
    {0x0C,0x18,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00,0x00,0x00}, /* 193 A acut */
    {0x10,0x38,0x6C,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00,0x00,0x00}, /* 194 A circ */
    {0x76,0xDC,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00,0x00,0x00}, /* 195 A tild */
    {0xC6,0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00,0x00,0x00}, /* 196 A diar */
    {0x38,0x6C,0x38,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00,0x00,0x00}, /* 197 A ring */
    {0x00,0x00,0x3E,0x6C,0xCC,0xCC,0xFE,0xCC,0xCC,0xCC,0xCC,0xCE,0x00,0x00,0x00}, /* 198 AE */
    {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0x18,0x70,0x00}, /* 199 C cedil */
    {0x60,0x30,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x62,0x66,0xFE,0x00,0x00,0x00}, /* 200 E grav */
    {0x0C,0x18,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x62,0x66,0xFE,0x00,0x00,0x00}, /* 201 E acut */
    {0x10,0x38,0x6C,0xFE,0x66,0x62,0x68,0x78,0x68,0x62,0x66,0xFE,0x00,0x00,0x00}, /* 202 E circ */
    {0xC6,0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x62,0x66,0xFE,0x00,0x00,0x00}, /* 203 E diar */
    {0x60,0x30,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00}, /* 204 I grav */
    {0x0C,0x18,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00}, /* 205 I acut */
    {0x18,0x3C,0x66,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00}, /* 206 I circ */
    {0x66,0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00}, /* 207 I diar */
    {0x00,0x00,0xF8,0x6C,0x66,0x66,0xF6,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00}, /* 208 Eth */
    {0x76,0xDC,0x00,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0x00,0x00,0x00}, /* 209 N tild */
    {0x60,0x30,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 210 O grav */
    {0x0C,0x18,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 211 O acut */
    {0x10,0x38,0x6C,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 212 O circ */
    {0x76,0xDC,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 213 O tild */
    {0xC6,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 214 O diar */
    {0x00,0x00,0x00,0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00}, /* 215 mult */
    {0x00,0x00,0x06,0x7C,0xCE,0xDE,0xD6,0xF6,0xE6,0xC6,0x7C,0xC0,0x00,0x00,0x00}, /* 216 O strk */
    {0x60,0x30,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 217 U grav */
    {0x0C,0x18,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 218 U acut */
    {0x10,0x38,0x6C,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 219 U circ */
    {0xC6,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 220 U diar */
    {0x0C,0x18,0x00,0xC3,0xC3,0x66,0x66,0x3C,0x18,0x18,0x18,0x3C,0x00,0x00,0x00}, /* 221 Y acut */
    {0x00,0x00,0xF0,0x60,0x7C,0x66,0x66,0x66,0x66,0x7C,0x60,0xF0,0x00,0x00,0x00}, /* 222 Thorn */
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xFC,0xC6,0xC6,0xC6,0xD6,0xDC,0x80,0x00,0x00}, /* 223 sharp s */
    /* CP1252 224-255: lowercase accented (32 characters) */
    {0x00,0x60,0x30,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00}, /* 224 a grav */
    {0x00,0x0C,0x18,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00}, /* 225 a acut */
    {0x10,0x38,0x6C,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00}, /* 226 a circ */
    {0x00,0x76,0xDC,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00}, /* 227 a tild */
    {0x00,0xCC,0x00,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00}, /* 228 a diar */
    {0x38,0x6C,0x38,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00}, /* 229 a ring */
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xD6,0x7C,0xD0,0xD0,0xD6,0x7C,0x00,0x00,0x00}, /* 230 ae */
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x18,0x70,0x00}, /* 231 c cedil */
    {0x00,0x60,0x30,0x00,0x00,0x7C,0xC6,0xFE,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00}, /* 232 e grav */
    {0x00,0x0C,0x18,0x00,0x00,0x7C,0xC6,0xFE,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00}, /* 233 e acut */
    {0x10,0x38,0x6C,0x00,0x00,0x7C,0xC6,0xFE,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00}, /* 234 e circ */
    {0x00,0xC6,0x00,0x00,0x00,0x7C,0xC6,0xFE,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00}, /* 235 e diar */
    {0x00,0x60,0x30,0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00}, /* 236 i grav */
    {0x00,0x0C,0x18,0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00}, /* 237 i acut */
    {0x18,0x3C,0x66,0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00}, /* 238 i circ */
    {0x00,0x66,0x00,0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00}, /* 239 i diar */
    {0x00,0x76,0x1C,0x3C,0x60,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 240 eth */
    {0x00,0x76,0xDC,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00}, /* 241 n tild */
    {0x00,0x60,0x30,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 242 o grav */
    {0x00,0x0C,0x18,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 243 o acut */
    {0x10,0x38,0x6C,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 244 o circ */
    {0x00,0x76,0xDC,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 245 o tild */
    {0x00,0xC6,0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00}, /* 246 o diar */
    {0x00,0x00,0x00,0x00,0x18,0x00,0x7E,0x00,0x18,0x00,0x00,0x00,0x00,0x00,0x00}, /* 247 divis */
    {0x00,0x00,0x00,0x00,0x00,0x06,0x7C,0xDE,0xF6,0xE6,0x7C,0xC0,0x00,0x00,0x00}, /* 248 o strk */
    {0x00,0x60,0x30,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00}, /* 249 u grav */
    {0x00,0x0C,0x18,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00}, /* 250 u acut */
    {0x10,0x38,0x6C,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00}, /* 251 u circ */
    {0x00,0xCC,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00}, /* 252 u diar */
    {0x00,0x0C,0x18,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0xF8}, /* 253 y acut */
    {0x00,0x00,0xF0,0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, /* 254 thorn */
    {0x00,0xCC,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0xF8}, /* 255 y diar */
};

/* Forward declarations */
static void locate(int x, int y);
static void print_char(char c);
static void print_string(const char *str);
static void clear_screen(void);
static void update_display(void);
static int read_keyboard_status(void);
static int file_exists(const char *pathname);
static char *get_line(FILE *fp);

/* Macros */
#define IMAGE_AREA_PIXELS (SCREEN_WIDTH * TEXT_AREA_START)
#define TEXT_AREA_PIXELS (SCREEN_WIDTH * 80)
#define RestoreScreen() memcpy(g_videoram, g_background, IMAGE_AREA_PIXELS * sizeof(uint32_t))
#define SaveScreen() memcpy(g_background, g_videoram, IMAGE_AREA_PIXELS * sizeof(uint32_t))

#define SaveMacro() {\
    FILE *fd = fopen(savefile, "w");\
    RestoreScreen();\
    if (fd != NULL) {\
        int _err = 0;\
        _err |= fprintf(fd, "%06ld%d%d%d%d%d%d%d%d%d%d\n", savepointer,\
            choicedata[0], choicedata[1], choicedata[2], choicedata[3], choicedata[4],\
            choicedata[5], choicedata[6], choicedata[7], choicedata[8], choicedata[9]) < 0;\
        _err |= fprintf(fd, "%d\n", savehistory_idx) < 0;\
        for (int i = 0; i < savehistory_idx; i++) {\
            _err |= fprintf(fd, "%d\n", savehistory[i]) < 0;\
        }\
        _err |= fclose(fd) != 0;\
        if (_err) DispSaveError();\
    } else {\
        DispSaveError();\
    }\
}

#define FlushMessages() {\
    MSG msg;\
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {\
        TranslateMessage(&msg);\
        DispatchMessage(&msg);\
    }\
}

#define HandleSaveFilename(n) {\
    if ((n) == 19) {\
        snprintf(savefile, 14, "data\\sav0.sav");\
    } else {\
        snprintf(savefile, 14, "data\\sav%d.sav", (n) - 9);\
    }\
}

#define ParseVCommand() {\
    if (strlen(line) == 3) {\
        char registername_s[2] = {0};\
        memcpy(registername_s, line+1, 1);\
        char registervalue_s[2] = {0};\
        memcpy(registervalue_s, line+2, 1);\
        int registername_i = atoi(registername_s);\
        int registervalue_i = atoi(registervalue_s);\
        if (registername_i >= 0 && registername_i < 10 && registervalue_i >= 0 && registervalue_i < 10) {\
            choicedata[registername_i] = (char)registervalue_i;\
        }\
    }\
}

#define DispEraseError() {\
    RestoreScreen();\
    locate(0, 0);\
    print_string("Delete failed! Press Space...");\
    update_display();\
    while (read_keyboard_status() != 1 && g_running) {\
        Sleep(5);\
    }\
}

#define DispSaveError() {\
    locate(0, 0);\
    print_string("Save failed! Press Space...");\
    update_display();\
    while (read_keyboard_status() != 1 && g_running) {\
        Sleep(5);\
    }\
    RestoreScreen();\
}

#define DeleteMacro() {\
    next = read_keyboard_status();\
    while (NoValidSaveChoice(next) && g_running) {\
        if (next == 7) RestoreWindowSize();\
        next = read_keyboard_status();\
        Sleep(5);\
    }\
    if (next != 2) {\
        HandleSaveFilename(next);\
        if(file_exists(savefile) == 0) {\
            if (remove(savefile) != 0) {\
                DispEraseError();\
            }\
        }\
    }\
}

/* Draw a vertical line */
static void DrawVLine(int x1, int y1, int y2) {
    for (int y = y1; y <= y2; y++) {
        g_videoram[y * SCREEN_WIDTH + x1] = COLOR_BLACK;
    }
}

/* Draw a horizontal line */
static void DrawHLine(int x1, int y1, int x2) {
    uint32_t *ptr = g_videoram + y1 * SCREEN_WIDTH + x1;
    for (int x = x1; x <= x2; x++) {
        *ptr++ = COLOR_BLACK;
    }
}

static void RedrawBorder(void) {
    DrawHLine(0, 320, 640);
    DrawHLine(0, 399, 640);
    DrawVLine(0, 320, 399);
    DrawVLine(639, 320, 399);
}

/* Set cursor position (in character cells, 8x8 font) */
static void locate(int x, int y) {
    g_cursorX = x;
    g_cursorY = y;
}

/* Draw a single character at the current cursor position
 * Uses 8x15 VGA-style font
 * Text area (y >= 320) has 1 pixel offset for gap after border */
static void print_char(char c) {
    unsigned char uc = (unsigned char)c;
    int idx;

    /* ASCII 32-127 maps to font index 0-95 */
    /* CP1252 128-255 maps to font index 96-223 */
    if (uc < 32) return;
    idx = uc - 32;

    /* g_cursorX and g_cursorY are now pixel coordinates */
    int px = g_cursorX;
    int py = g_cursorY;

    /* Text area (y >= 320) gets 2 pixel right shift */
    if (py >= TEXT_AREA_START) {
        px += 2;
    }

    if (px >= SCREEN_WIDTH || py >= SCREEN_HEIGHT) return;

    for (int row = 0; row < 15; row++) {
        int screen_row = py + row;
        if (screen_row >= SCREEN_HEIGHT) break;

        uint8_t glyph_row = g_font8x15[idx][row];
        uint32_t *pixel = g_videoram + screen_row * SCREEN_WIDTH + px;

        /* Write 8 pixels for this row of the glyph */
        for (int bit = 0; bit < 8; bit++) {
            if (glyph_row & (0x80 >> bit)) {
                pixel[bit] = COLOR_BLACK;
            } else {
                pixel[bit] = COLOR_WHITE;
            }
        }
    }
    g_cursorX += 8;
}

/* Print a string */
static void print_string(const char *str) {
    while (*str) {
        if (*str == '\n') {
            g_cursorX = 0;
            /* Text area uses 15-pixel line height, image area uses 16 */
            g_cursorY += (g_cursorY >= TEXT_AREA_START) ? 15 : 16;
        } else if (*str == '\r') {
            g_cursorX = 0;
        } else {
            print_char(*str);
        }
        str++;
    }
}

/* Clear the entire screen to white */
static void clear_screen(void) {
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        g_videoram[i] = COLOR_WHITE;
    }
    g_cursorX = 0;
    g_cursorY = 0;
}

/* SCALED_RENDERING -> Scale rendering to 1280x800 (upscale x2) with hybrid filtering:
   bilinear for image section and text section
   nearest-neighbor for the text area border */
#ifdef SCALED_RENDERING
/* Nearest-neighbor scale a row */
static void nn_row(uint32_t *src_row, uint32_t *dst_row, int dst_start, int dst_end,
                   uint32_t x_ratio) {
    for (int x = dst_start; x < dst_end; x++) {
        dst_row[x] = src_row[(x * x_ratio) >> 16];
    }
}

/* Bilinear scale a row segment */
static void bilinear_row(uint32_t *row0, uint32_t *row1, uint32_t *dst_row,
                         int dst_start, int dst_end, int src_w,
                         uint32_t x_ratio, uint32_t fy) {
    uint32_t ify = 256 - fy;
    for (int x = dst_start; x < dst_end; x++) {
        uint32_t src_xf = x * x_ratio;
        int x0 = src_xf >> 16;
        int x1 = (x0 < src_w - 1) ? x0 + 1 : x0;
        uint32_t fx = (src_xf >> 8) & 0xFF;
        uint32_t ifx = 256 - fx;

        uint32_t p00 = row0[x0], p01 = row0[x1];
        uint32_t p10 = row1[x0], p11 = row1[x1];

        uint32_t b = (((p00 & 0xFF) * ifx + (p01 & 0xFF) * fx) * ify +
                      ((p10 & 0xFF) * ifx + (p11 & 0xFF) * fx) * fy) >> 16;
        uint32_t g = ((((p00 >> 8) & 0xFF) * ifx + ((p01 >> 8) & 0xFF) * fx) * ify +
                      (((p10 >> 8) & 0xFF) * ifx + ((p11 >> 8) & 0xFF) * fx) * fy) >> 16;
        uint32_t r = ((((p00 >> 16) & 0xFF) * ifx + ((p01 >> 16) & 0xFF) * fx) * ify +
                      (((p10 >> 16) & 0xFF) * ifx + ((p11 >> 16) & 0xFF) * fx) * fy) >> 16;

        dst_row[x] = 0xFF000000 | (r << 16) | (g << 8) | b;
    }
}

/* Per-pixel hybrid row: NN near borders, bilinear elsewhere */
static void hybrid_row(uint32_t *row_nn, uint32_t *row0, uint32_t *row1, uint32_t *dst_row,
                       int dst_w, int src_w, uint32_t x_ratio_nn, uint32_t x_ratio_bl,
                       uint32_t fy, int left_end, int right_start) {
    uint32_t ify = 256 - fy;
    for (int x = 0; x < dst_w; x++) {
        if (x < left_end || x >= right_start) {
            dst_row[x] = row_nn[(x * x_ratio_nn) >> 16];
        } else {
            uint32_t src_xf = x * x_ratio_bl;
            int x0 = src_xf >> 16;
            int x1 = (x0 < src_w - 1) ? x0 + 1 : x0;
            uint32_t fx = (src_xf >> 8) & 0xFF;
            uint32_t ifx = 256 - fx;

            uint32_t p00 = row0[x0], p01 = row0[x1];
            uint32_t p10 = row1[x0], p11 = row1[x1];

            uint32_t b = (((p00 & 0xFF) * ifx + (p01 & 0xFF) * fx) * ify +
                          ((p10 & 0xFF) * ifx + (p11 & 0xFF) * fx) * fy) >> 16;
            uint32_t g = ((((p00 >> 8) & 0xFF) * ifx + ((p01 >> 8) & 0xFF) * fx) * ify +
                          (((p10 >> 8) & 0xFF) * ifx + ((p11 >> 8) & 0xFF) * fx) * fy) >> 16;
            uint32_t r = ((((p00 >> 16) & 0xFF) * ifx + ((p01 >> 16) & 0xFF) * fx) * ify +
                          (((p10 >> 16) & 0xFF) * ifx + ((p11 >> 16) & 0xFF) * fx) * fy) >> 16;

            dst_row[x] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }
}

/* Scale framebuffer: bilinear everywhere, nearest-neighbor for text box border */
static void hybrid_scale(uint32_t *src, int src_w, int src_h,
                         uint32_t *dst, int dst_w, int dst_h) {
    uint32_t x_ratio_bl = ((src_w - 1) << 16) / dst_w;
    uint32_t y_ratio_bl = ((src_h - 1) << 16) / dst_h;
    uint32_t x_ratio_nn = (src_w << 16) / dst_w;
    uint32_t y_ratio_nn = (src_h << 16) / dst_h;

    /* Text area boundaries in flipped buffer */
    int text_top_flipped = src_h - TEXT_AREA_START - 1;
    int h_border_margin = 2;  /* Margin for horizontal borders (top/bottom) */
    int v_border_width = 1;   /* Actual vertical border width (left/right) */

    /* Destination Y boundaries */
    int text_area_end = ((src_h - TEXT_AREA_START) * dst_h) / src_h;
    int bot_border_end = ((h_border_margin + 1) * dst_h) / src_h;
    int top_border_start = ((text_top_flipped - h_border_margin) * dst_h) / src_h;
    int top_border_end = ((text_top_flipped + h_border_margin + 1) * dst_h) / src_h;
    /* Clamp top border to not extend into image area */
    if (top_border_end > text_area_end) top_border_end = text_area_end;

    /* Destination X boundaries for left/right borders (narrow - just the border itself) */
    int left_end = ((v_border_width + 1) * dst_w) / src_w;
    int right_start = ((src_w - v_border_width) * dst_w) / src_w;

    /* First and last text content rows (for per-pixel branching) */
    int first_text_row = bot_border_end;
    int last_text_row = top_border_start - 1;

    for (int y = 0; y < dst_h; y++) {
        uint32_t *out = dst + y * dst_w;
        int src_y_nn = (y * y_ratio_nn) >> 16;
        uint32_t *row_nn = src + src_y_nn * src_w;

        uint32_t src_yf = y * y_ratio_bl;
        int y0 = src_yf >> 16;
        int y1 = (y0 < src_h - 1) ? y0 + 1 : y0;
        uint32_t fy = (src_yf >> 8) & 0xFF;
        uint32_t *row0 = src + y0 * src_w;
        uint32_t *row1 = src + y1 * src_w;

        if (y < bot_border_end || (y >= top_border_start && y < top_border_end)) {
            /* Horizontal border rows: all nearest-neighbor */
            nn_row(row_nn, out, 0, dst_w, x_ratio_nn);
        } else if (y == first_text_row || y == last_text_row) {
            /* First/last text rows: per-pixel branching */
            hybrid_row(row_nn, row0, row1, out, dst_w, src_w,
                       x_ratio_nn, x_ratio_bl, fy, left_end, right_start);
        } else if (y < text_area_end) {
            /* Text content rows: NN for left/right borders, bilinear for middle */
            nn_row(row_nn, out, 0, left_end, x_ratio_nn);
            bilinear_row(row0, row1, out, left_end, right_start, src_w, x_ratio_bl, fy);
            nn_row(row_nn, out, right_start, dst_w, x_ratio_nn);
        } else {
            /* Image area: all bilinear */
            bilinear_row(row0, row1, out, 0, dst_w, src_w, x_ratio_bl, fy);
        }
    }
}

/* Update the Windows display from our framebuffer */
static void update_display(void) {
    if (!g_hwnd || !g_videoram) return;

    RECT rect;
    GetClientRect(g_hwnd, &rect);
    int dst_w = rect.right;
    int dst_h = rect.bottom;

    if (dst_w <= 0 || dst_h <= 0) return;

    /* Allocate scaled buffer */
    uint32_t *scaled = (uint32_t *)malloc(dst_w * dst_h * sizeof(uint32_t));
    if (!scaled) return;

    /* Flip source for bottom-up DIB format, then scale with bicubic */
    uint32_t *flipped = (uint32_t *)malloc(SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint32_t));
    if (!flipped) {
        free(scaled);
        return;
    }

    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        memcpy(flipped + y * SCREEN_WIDTH,
               g_videoram + (SCREEN_HEIGHT - 1 - y) * SCREEN_WIDTH,
               SCREEN_WIDTH * sizeof(uint32_t));
    }

    /* Apply hybrid scaling: bilinear for image, nearest-neighbor for text */
    hybrid_scale(flipped, SCREEN_WIDTH, SCREEN_HEIGHT, scaled, dst_w, dst_h);

    /* Use 32-bit DIB (BI_RGB) for Win32s compatibility */
    BITMAPINFOHEADER bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.biSize = sizeof(BITMAPINFOHEADER);
    bmi.biWidth = dst_w;
    bmi.biHeight = dst_h; /* Positive = bottom-up DIB */
    bmi.biPlanes = 1;
    bmi.biBitCount = 32;
    bmi.biCompression = BI_RGB;
    bmi.biSizeImage = 0;

    HDC hdc = GetDC(g_hwnd);
    if (hdc) {
        /* No stretching needed - scaled buffer matches window size */
        SetDIBitsToDevice(hdc, 0, 0, dst_w, dst_h,
                          0, 0, 0, dst_h,
                          scaled, (BITMAPINFO *)&bmi, DIB_RGB_COLORS);
        ReleaseDC(g_hwnd, hdc);
    }

    free(flipped);
    free(scaled);
}

/* Restore window to 1280x800 size */
static void RestoreWindowSize(void) {
    RECT rect = {0, 0, 1280, 800};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(g_hwnd, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER);
}

/* Default to non-scaled rendering: 640x400 screen with nearest-neighbor interpolation when resizing the window */
#else
/* Update the Windows display from our framebuffer */
static void update_display(void) {
    if (!g_hwnd || !g_videoram) return;

    /* Use 32-bit DIB (BI_RGB) for Win32s compatibility */
    BITMAPINFOHEADER bmi;

    memset(&bmi, 0, sizeof(bmi));
    bmi.biSize = sizeof(BITMAPINFOHEADER);
    bmi.biWidth = SCREEN_WIDTH;
    bmi.biHeight = SCREEN_HEIGHT; /* Positive = bottom-up DIB */
    bmi.biPlanes = 1;
    bmi.biBitCount = 32;  /* 32-bit color */
    bmi.biCompression = BI_RGB;
    bmi.biSizeImage = 0;  /* Can be 0 for BI_RGB */

    /* Flip rows for bottom-up DIB format */
    uint32_t *flipped = (uint32_t *)malloc(SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint32_t));
    if (!flipped) return;

    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        memcpy(flipped + y * SCREEN_WIDTH,
               g_videoram + (SCREEN_HEIGHT - 1 - y) * SCREEN_WIDTH,
               SCREEN_WIDTH * sizeof(uint32_t));
    }

    HDC hdc = GetDC(g_hwnd);
    if (hdc) {
        RECT rect;
        GetClientRect(g_hwnd, &rect);
        StretchDIBits(hdc, 0, 0, rect.right, rect.bottom,
                      0, 0, SCREEN_WIDTH, SCREEN_HEIGHT,
                      flipped, (BITMAPINFO *)&bmi, DIB_RGB_COLORS, SRCCOPY);
        ReleaseDC(g_hwnd, hdc);
    }

    free(flipped);
}

/* Restore window to original 640x400 size */
static void RestoreWindowSize(void) {
    RECT rect = {0, 0, SCREEN_WIDTH, SCREEN_HEIGHT};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(g_hwnd, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER);
}
#endif

/* Check if a file exists */
static int file_exists(const char *pathname) {
    DWORD attr = GetFileAttributesA(pathname);
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) ? 0 : -1;
}

/* Read a line from file */
static char *get_line(FILE *fp) {
    static char newline[300] = {0};
    if (fp == NULL) return NULL;

    newline[299] = '\0';
    if (fgets(newline, 300, fp) == NULL) return NULL;

    size_t len = strlen(newline);
    if (len > 0 && newline[len - 1] == '\n') newline[len - 1] = '\0';
    if (len > 1 && newline[len - 2] == '\r') newline[len - 2] = '\0';

    return newline;
}

/* Non-blocking keyboard check */
static int read_keyboard_status(void) {
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            g_running = 0;
            return 2; /* Quit */
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (!g_running) return 2;

    int key = g_lastkey;
    g_lastkey = 0;
    return key;
}

/* Sprite structure */
typedef struct {
    int x;
    int y;
    char file[260];
} sprite;

static sprite currentsprites[256];
static sprite previoussprites[256];

static void backup_spritearray(void) {
    for (int i = 0; i <= 255; i++) {
        previoussprites[i].x = currentsprites[i].x;
        previoussprites[i].y = currentsprites[i].y;
        if (strlen(currentsprites[i].file) > 0) {
            memcpy(previoussprites[i].file, currentsprites[i].file, 260);
        } else {
            memset(previoussprites[i].file, 0, 260);
        }
    }
}

static void reset_cursprites(void) {
    for (int i = 0; i <= 255; i++) {
        currentsprites[i].x = 0;
        currentsprites[i].y = 0;
        memset(currentsprites[i].file, 0, 260);
    }
}

static void reset_prevsprites(void) {
    for (int i = 0; i <= 255; i++) {
        previoussprites[i].x = 0;
        previoussprites[i].y = 0;
        memset(previoussprites[i].file, 0, 260);
    }
}

static int compare_sprites(void) {
    for (int i = 0; i <= 255; i++) {
        if (currentsprites[i].x != previoussprites[i].x) return 1;
        if (currentsprites[i].y != previoussprites[i].y) return 1;
        if (strncmp(currentsprites[i].file, previoussprites[i].file, 260) != 0) return 1;
    }
    return 0;
}

/* Display Load/Save dialog */
static void DispLoadSave(int mode) {
    char savepath[15] = {0};

    /* Fill dialog area with white - 161x129 centered in 640x320 */
    for (int y = 96; y <= 224; y++) {
        for (int x = 240; x <= 400; x++) {
            g_videoram[y * SCREEN_WIDTH + x] = COLOR_WHITE;
        }
    }

    if(mode == 0) { locate(277, 96); print_string("- Loading -"); }
    if(mode == 1) { locate(281, 96); print_string("- Saving -"); }
    if(mode == 2) { locate(281, 96); print_string("- Delete -"); }

    DrawHLine(240, 96, 400);
    DrawHLine(240, 224, 400);
    DrawVLine(240, 96, 224);
    DrawVLine(400, 96, 224);

    /* Left column: 1-5 */
    for (int i = 1; i <= 5; i++) {
        locate(248, 96 + i * 16);
        snprintf(savepath, 15, "data\\sav%d.sav", i);
        if (file_exists(savepath) == 0) {
            print_char('0' + i);
            print_string(": USED ");
        } else {
            print_char('0' + i);
            print_string(": EMPTY");
        }
    }

    /* Right column: 6-9, 0 */
    for (int i = 6; i <= 9; i++) {
        locate(328, (1 + i) * 16);
        snprintf(savepath, 15, "data\\sav%d.sav", i);
        if (file_exists(savepath) == 0) {
            print_char('0' + i);
            print_string(": USED ");
        } else {
            print_char('0' + i);
            print_string(": EMPTY");
        }
    }

    locate(328, 176);
    if (file_exists("data\\sav0.sav") == 0) {
        print_string("0: USED ");
    } else {
        print_string("0: EMPTY");
    }

    locate(280, 208);
    print_string("[q] : quit");

    update_display();
}

/* Display help dialog */
static void DispHelp(void) {
    /* Fill dialog area with white - 140x129 centered in 640x320 */
    for (int y = 96; y <= 224; y++) {
        for (int x = 250; x <= 389; x++) {
            g_videoram[y * SCREEN_WIDTH + x] = COLOR_WHITE;
        }
    }

    locate(252, 96);
    print_string("-     Usage     -");
    locate(252, 112);
    print_string("[q] Quit         ");
    locate(252, 128);
    print_string("[b] Back         ");
    locate(252, 144);
    print_string("[l] Load save    ");
    locate(252, 160);
    print_string("[s] Save state   ");
    locate(252, 176);
    print_string("[e] Erase save   ");
    locate(252, 192);
    print_string("[r] Restore size ");
    locate(252, 208);
    print_string("[ ] Advance      ");

    DrawHLine(250, 96, 389);
    DrawHLine(250, 224, 389);
    DrawVLine(250, 96, 224);
    DrawVLine(389, 96, 224);

    update_display();
}

/* Delay function compatible with Win32s
 * Uses timeGetTime() from winmm.dll for ~1ms resolution */
static void FxDelay(DWORD ms) {
    DWORD start, elapsed;
    MSG msg;

    start = timeGetTime();

    while (1) {
        /* Pump messages to keep window responsive */
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        elapsed = timeGetTime() - start;
        if (elapsed >= ms) {
            break;
        }
    }
}

/* Helper to fill a row with a color */
static void FillRow(int y, uint32_t color) {
    uint32_t *row = g_videoram + y * SCREEN_WIDTH;
    for (int x = 0; x < SCREEN_WIDTH; x++) {
        row[x] = color;
    }
}

/* Fill multiple rows at once for faster effects */
static void FillRows(int y, int count, uint32_t color) {
    for (int i = 0; i < count; i++) {
        int row = y + i;
        if (row >= 0 && row < 320) FillRow(row, color);
    }
}

/* Screen transition effects */
static void FxVWipeDown(uint32_t color) {
    for (int i = 0; i < 320; i += 8) {
        FillRows(i, 8, color);
        update_display();
        FxDelay(15);
    }
}

static void FxVWipeUp(uint32_t color) {
    for (int i = 312; i >= 0; i -= 8) {
        FillRows(i, 8, color);
        update_display();
        FxDelay(15);
    }
}

static void FxVWipeMidIn(uint32_t color) {
    /* Wipe from edges (0 and 319) toward center (160) */
    for (int i = 0; i < 160; i += 8) {
        FillRows(i, 8, color);                /* Top edge moving down */
        FillRows(312 - i, 8, color);          /* Bottom edge moving up */
        update_display();
        FxDelay(15);
    }
}

static void FxVWipeMidOut(uint32_t color) {
    /* Wipe from center (160) toward edges */
    for (int i = 0; i < 160; i += 8) {
        FillRows(160 + i, 8, color);          /* Center moving down */
        FillRows(152 - i, 8, color);          /* Center moving up */
        update_display();
        FxDelay(15);
    }
}

static void FxHWipeRight(uint32_t color) {
    /* Wipe 32 pixels at a time for speed */
    for (int col = 0; col < SCREEN_WIDTH; col += 32) {
        for (int line = 0; line < 320; line++) {
            uint32_t *row = g_videoram + line * SCREEN_WIDTH + col;
            for (int p = 0; p < 32 && col + p < SCREEN_WIDTH; p++) {
                row[p] = color;
            }
        }
        update_display();
        FxDelay(15);
    }
}

static void FxHWipeLeft(uint32_t color) {
    /* Wipe 32 pixels at a time for speed */
    for (int col = SCREEN_WIDTH - 32; col >= 0; col -= 32) {
        for (int line = 0; line < 320; line++) {
            uint32_t *row = g_videoram + line * SCREEN_WIDTH + col;
            for (int p = 0; p < 32; p++) {
                row[p] = color;
            }
        }
        update_display();
        FxDelay(15);
    }
}

static void FxHWipeMidIn(uint32_t color) {
    /* 64 pixels at a time (32 from each side) */
    for (int col = 0; col < SCREEN_WIDTH / 2; col += 32) {
        for (int line = 0; line < 320; line++) {
            uint32_t *row = g_videoram + line * SCREEN_WIDTH;
            for (int p = 0; p < 32 && col + p < SCREEN_WIDTH / 2; p++) {
                row[col + p] = color;
                row[SCREEN_WIDTH - 1 - col - p] = color;
            }
        }
        update_display();
        FxDelay(15);
    }
}

static void FxHWipeMidOut(uint32_t color) {
    /* 64 pixels at a time (32 from center to each side) */
    for (int col = 0; col < SCREEN_WIDTH / 2; col += 32) {
        for (int line = 0; line < 320; line++) {
            uint32_t *row = g_videoram + line * SCREEN_WIDTH;
            for (int p = 0; p < 32 && col + p < SCREEN_WIDTH / 2; p++) {
                row[SCREEN_WIDTH / 2 - 1 - col - p] = color;
                row[SCREEN_WIDTH / 2 + col + p] = color;
            }
        }
        update_display();
        FxDelay(15);
    }
}

/* Fill a 16-pixel wide block at position (bx*16, y) */
static void FillBlock16(int bx, int y, uint32_t color) {
    if (bx < 0 || bx >= 40 || y < 0 || y >= 320) return;
    uint32_t *ptr = g_videoram + y * SCREEN_WIDTH + bx * 16;
    for (int p = 0; p < 16; p++) {
        ptr[p] = color;
    }
}

static void FxCircleOut(uint32_t color) {
    int bcx = 20;
    int bcy = 5;

    /* Process 3 radii at a time for speed */
    for (int r = 0; r <= 23; r += 3) {
        int r_end = (r + 2 <= 23) ? r + 2 : 23;
        int r2 = r_end * r_end;
        int prev_r2 = (r > 0) ? (r - 1) * (r - 1) : -1;

        for (int by = 0; by < 10; by++) {
            int dy = by - bcy;
            int dy2 = 4 * dy * dy;
            if (dy2 > r2) continue;

            int dx = 0;
            int target = r2 - dy2;
            while ((dx + 1) * (dx + 1) <= target) dx++;

            int prev_dx = -1;
            if (prev_r2 >= 0 && dy2 <= prev_r2) {
                prev_dx = 0;
                int prev_target = prev_r2 - dy2;
                while ((prev_dx + 1) * (prev_dx + 1) <= prev_target) prev_dx++;
            }

            for (int line = 0; line < 32; line++) {
                int y = by * 32 + line;
                if (y >= 320) continue;

                for (int bx = bcx - dx; bx <= bcx - prev_dx - 1; bx++) {
                    FillBlock16(bx, y, color);
                }

                for (int bx = bcx + prev_dx + 1; bx <= bcx + dx; bx++) {
                    FillBlock16(bx, y, color);
                }
            }
        }
        update_display();
        FxDelay(40);
    }
}

static void FxCircleIn(uint32_t color) {
    int bcx = 20;
    int bcy = 5;

    /* Process 3 radii at a time for speed */
    for (int r = 23; r >= 0; r -= 3) {
        int r2 = r * r;
        int r_end = (r - 2 >= 0) ? r - 2 : 0;
        int next_r2 = (r_end > 0) ? (r_end - 1) * (r_end - 1) : -1;

        for (int by = 0; by < 10; by++) {
            int dy = by - bcy;
            int dy2 = 4 * dy * dy;
            if (dy2 > r2) continue;

            int dx = 0;
            int target = r2 - dy2;
            while ((dx + 1) * (dx + 1) <= target) dx++;

            int next_dx = -1;
            if (next_r2 >= 0 && dy2 <= next_r2) {
                next_dx = 0;
                int next_target = next_r2 - dy2;
                while ((next_dx + 1) * (next_dx + 1) <= next_target) next_dx++;
            }

            for (int line = 0; line < 32; line++) {
                int y = by * 32 + line;
                if (y >= 320) continue;

                for (int bx = bcx - dx; bx <= bcx - next_dx - 1; bx++) {
                    FillBlock16(bx, y, color);
                }

                for (int bx = bcx + next_dx + 1; bx <= bcx + dx; bx++) {
                    FillBlock16(bx, y, color);
                }
            }
        }
        update_display();
        FxDelay(40);
    }
}

/* Load a PNG image into 32-bit BGRA buffer */
static int LoadPngImage(const char *filename, uint32_t *background) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) return -1;

    /* Check PNG signature */
    uint8_t header[8];
    if (fread(header, 1, 8, fp) != 8 || png_sig_cmp(header, 0, 8)) {
        fclose(fp);
        return -1;
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        return -1;
    }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        fclose(fp);
        return -1;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return -1;
    }

    png_init_io(png, fp);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);

    png_uint_32 width = png_get_image_width(png, info);
    png_uint_32 height = png_get_image_height(png, info);
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth = png_get_bit_depth(png, info);

    /* Set up transforms to get 8-bit RGB */
    if (bit_depth == 16)
        png_set_strip_16(png);

    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);

    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);

    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);

    /* Expand grayscale to RGB */
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }

    /* Strip alpha channel - we don't need it */
    if (color_type & PNG_COLOR_MASK_ALPHA)
        png_set_strip_alpha(png);

    /* Request BGR order for Win32 DIB compatibility */
    png_set_bgr(png);

    png_read_update_info(png, info);

    /* Allocate row pointers */
    png_bytep *row_pointers = (png_bytep *)malloc(sizeof(png_bytep) * height);
    if (!row_pointers) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return -1;
    }

    size_t rowbytes = png_get_rowbytes(png, info);
    for (png_uint_32 y = 0; y < height; y++) {
        row_pointers[y] = (png_byte *)malloc(rowbytes);
        if (!row_pointers[y]) {
            for (png_uint_32 j = 0; j < y; j++) free(row_pointers[j]);
            free(row_pointers);
            png_destroy_read_struct(&png, &info, NULL);
            fclose(fp);
            return -1;
        }
    }

    png_read_image(png, row_pointers);
    png_read_end(png, NULL);
    fclose(fp);

    /* Clear background buffer to white */
    for (int i = 0; i < SCREEN_WIDTH * TEXT_AREA_START; i++) {
        background[i] = COLOR_WHITE;
    }

    /* Copy pixels to 32-bit BGRA buffer */
    /* Limit to screen dimensions (640x320 for image area) */
    png_uint_32 copy_width = (width > SCREEN_WIDTH) ? SCREEN_WIDTH : width;
    png_uint_32 copy_height = (height > TEXT_AREA_START) ? TEXT_AREA_START : height;

    for (png_uint_32 y = 0; y < copy_height; y++) {
        png_byte *row = row_pointers[y];
        for (png_uint_32 x = 0; x < copy_width; x++) {
            /* Row is BGR format (3 bytes per pixel) */
            uint8_t b = row[x * 3 + 0];
            uint8_t g = row[x * 3 + 1];
            uint8_t r = row[x * 3 + 2];
            /* BGRA format: 0xAARRGGBB in memory */
            background[y * SCREEN_WIDTH + x] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }

    /* Free row pointers */
    for (png_uint_32 y = 0; y < height; y++) {
        free(row_pointers[y]);
    }
    free(row_pointers);

    png_destroy_read_struct(&png, &info, NULL);
    return 0;
}

/* Check if file has PNG signature */
static int IsPngFile(const char *filename) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) return 0;

    uint8_t header[8];
    int is_png = 0;
    if (fread(header, 1, 8, fp) == 8) {
        is_png = (png_sig_cmp(header, 0, 8) == 0);
    }
    fclose(fp);
    return is_png;
}

/* Load a compressed background image (PI1/Degas format) and convert to 32-bit */
static int LoadBackgroundImagePI1(const char *picture, uint8_t *bgpalette, uint32_t *background) {
    /* Temporary buffer for monochrome data */
    uint8_t *mono = (uint8_t *)malloc(SCREEN_WIDTH * TEXT_AREA_START / 8);
    if (!mono) return -1;

    gzFile gzf = gzopen(picture, "rb");
    if (gzf == NULL) {
        free(mono);
        return -1;
    }

    gzseek(gzf, 2, SEEK_CUR);  /* Skip resolution word */
    gzread(gzf, bgpalette, 32); /* Read palette (unused in mono) */
    gzread(gzf, mono, SCREEN_WIDTH * TEXT_AREA_START / 8);
    gzclose(gzf);

    /* Convert monochrome to 32-bit BGRA */
    for (int y = 0; y < TEXT_AREA_START; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            int bytepos = (y * SCREEN_WIDTH + x) / 8;
            int bitpos = 7 - (x % 8);
            int bit = (mono[bytepos] >> bitpos) & 1;
            background[y * SCREEN_WIDTH + x] = bit ? COLOR_BLACK : COLOR_WHITE;
        }
    }

    free(mono);
    return 0;
}

/* Load a background image (auto-detects PNG or PI1 format) */
static int LoadBackgroundImage(const char *picture, uint8_t *bgpalette, uint32_t *background) {
    if (IsPngFile(picture)) {
        return LoadPngImage(picture, background);
    }
    return LoadBackgroundImagePI1(picture, bgpalette, background);
}

/* Display a PNG sprite with alpha transparency */
static int DisplayPngSprite(const char *filename, int posx, int posy) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) return -1;

    /* Check PNG signature */
    uint8_t header[8];
    if (fread(header, 1, 8, fp) != 8 || png_sig_cmp(header, 0, 8)) {
        fclose(fp);
        return -1;
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        return -1;
    }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        fclose(fp);
        return -1;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return -1;
    }

    png_init_io(png, fp);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);

    png_uint_32 width = png_get_image_width(png, info);
    png_uint_32 height = png_get_image_height(png, info);
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth = png_get_bit_depth(png, info);

    /* Set up transforms to get 8-bit RGBA */
    if (bit_depth == 16)
        png_set_strip_16(png);

    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);

    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);

    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);

    /* Expand grayscale to RGB */
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }

    /* Add alpha channel if missing */
    if (!(color_type & PNG_COLOR_MASK_ALPHA)) {
        png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    }

    /* Request BGR order for Win32 DIB compatibility */
    png_set_bgr(png);

    png_read_update_info(png, info);

    /* Allocate row pointers */
    png_bytep *row_pointers = (png_bytep *)malloc(sizeof(png_bytep) * height);
    if (!row_pointers) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return -1;
    }

    size_t rowbytes = png_get_rowbytes(png, info);
    for (png_uint_32 y = 0; y < height; y++) {
        row_pointers[y] = (png_byte *)malloc(rowbytes);
        if (!row_pointers[y]) {
            for (png_uint_32 j = 0; j < y; j++) free(row_pointers[j]);
            free(row_pointers);
            png_destroy_read_struct(&png, &info, NULL);
            fclose(fp);
            return -1;
        }
    }

    png_read_image(png, row_pointers);
    png_read_end(png, NULL);
    fclose(fp);

    /* Blend sprite onto videoram with alpha */
    for (png_uint_32 sy = 0; sy < height; sy++) {
        int screen_y = posy + sy;
        if (screen_y < 0) continue;
        if (screen_y >= TEXT_AREA_START) break;

        png_byte *row = row_pointers[sy];
        for (png_uint_32 sx = 0; sx < width; sx++) {
            int screen_x = posx + sx;
            if (screen_x < 0) continue;
            if (screen_x >= SCREEN_WIDTH) break;

            /* Row is BGRA format (4 bytes per pixel) */
            uint8_t b = row[sx * 4 + 0];
            uint8_t g = row[sx * 4 + 1];
            uint8_t r = row[sx * 4 + 2];
            uint8_t a = row[sx * 4 + 3];

            if (a == 0) {
                /* Fully transparent - skip */
                continue;
            }

            int ppos = screen_y * SCREEN_WIDTH + screen_x;

            if (a == 255) {
                /* Fully opaque - direct copy */
                g_videoram[ppos] = 0xFF000000 | (r << 16) | (g << 8) | b;
            } else {
                /* Alpha blend: result = src * alpha + dst * (255 - alpha) */
                uint32_t dst = g_videoram[ppos];
                uint8_t dst_b = dst & 0xFF;
                uint8_t dst_g = (dst >> 8) & 0xFF;
                uint8_t dst_r = (dst >> 16) & 0xFF;

                uint8_t out_r = (r * a + dst_r * (255 - a)) / 255;
                uint8_t out_g = (g * a + dst_g * (255 - a)) / 255;
                uint8_t out_b = (b * a + dst_b * (255 - a)) / 255;

                g_videoram[ppos] = 0xFF000000 | (out_r << 16) | (out_g << 8) | out_b;
            }
        }
    }

    /* Free row pointers */
    for (png_uint_32 y = 0; y < height; y++) {
        free(row_pointers[y]);
    }
    free(row_pointers);

    png_destroy_read_struct(&png, &info, NULL);
    return 0;
}

/* Display a text-based sprite (legacy format) */
static int DisplayTextSprite(const char *spritefile, int posx, int posy) {
    gzFile sprite = gzopen(spritefile, "rb");
    if (sprite == NULL) return -1;

    /* Get uncompressed size */
    FILE *fp = fopen(spritefile, "rb");
    if (!fp) {
        gzclose(sprite);
        return -1;
    }

    uint16_t header;
    fread(&header, 2, 1, fp);

    uint32_t pctsize;
    if (header == 0x8b1f) { /* gzip magic (little-endian) */
        fseek(fp, -4, SEEK_END);
        uint8_t bytes[4];
        fread(bytes, 4, 1, fp);
        pctsize = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
    } else {
        fseek(fp, 0, SEEK_END);
        pctsize = ftell(fp);
    }
    fclose(fp);

    char *pctmem = (char *)malloc(pctsize);
    if (!pctmem) {
        gzclose(sprite);
        return -1;
    }

    gzread(sprite, pctmem, pctsize);
    gzclose(sprite);

    int x = 0, y = 0;
    for (uint32_t pctpos = 0; pctpos < pctsize; pctpos++) {
        if (pctmem[pctpos] == 10) { /* Newline */
            if (posy + y <= SCREEN_HEIGHT) {
                y++;
                x = 0;
            } else {
                break;
            }
        } else if (pctmem[pctpos] == ' ') { /* Transparency */
            if (x + posx < 639) x++;
        } else if (pctmem[pctpos] == '0' || pctmem[pctpos] == '1') {
            if (x + posx < 639 && posy + y <= SCREEN_HEIGHT && posy + y < TEXT_AREA_START) {
                int ppos = (y + posy) * SCREEN_WIDTH + x + posx;
                g_videoram[ppos] = (pctmem[pctpos] == '1') ? COLOR_BLACK : COLOR_WHITE;
                x++;
            }
        }
    }

    free(pctmem);
    return 0;
}

/* Display a sprite (auto-detects PNG or legacy text format) */
static int DisplaySprite(const char *spritefile, int posx, int posy) {
    if (IsPngFile(spritefile)) {
        return DisplayPngSprite(spritefile, posx, posy);
    }
    return DisplayTextSprite(spritefile, posx, posy);
}

/* Window procedure */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_KEYDOWN:
            if (!g_effectrunning) {
                switch (wParam) {
                    case VK_SPACE: g_lastkey = 1; break;
                    case 'Q': g_lastkey = 2; break;
                    case 'S': g_lastkey = 3; break;
                    case 'L': g_lastkey = 4; break;
                    case 'B': g_lastkey = 5; break;
                    case 'H': g_lastkey = 6; break;
                    case 'R': g_lastkey = 7; break;
                    case 'E': g_lastkey = 8; break;
                    case '1': g_lastkey = 10; break;
                    case '2': g_lastkey = 11; break;
                    case '3': g_lastkey = 12; break;
                    case '4': g_lastkey = 13; break;
                    case '5': g_lastkey = 14; break;
                    case '6': g_lastkey = 15; break;
                    case '7': g_lastkey = 16; break;
                    case '8': g_lastkey = 17; break;
                    case '9': g_lastkey = 18; break;
                    case '0': g_lastkey = 19; break;
                }
            }
            break;

        case WM_LBUTTONDOWN:
            if (g_ignoreclick) {
                g_ignoreclick = 0;  /* Ignore the click that activated the window */
            } else if (g_windowactive) {
                g_mouseclick = 1;
            }
            break;

        case WM_NCLBUTTONDOWN:
            /* Track title bar click before activation */
            if (wParam == HTCAPTION && !g_windowactive) {
                g_titlebarclick = 1;
            }
            return DefWindowProc(hwnd, msg, wParam, lParam);

        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) {
                g_windowactive = 0;
            } else {
                g_windowactive = 1;
                SetFocus(hwnd);  /* Ensure keyboard focus on Win32s */
                if (LOWORD(wParam) == WA_CLICKACTIVE) {
                    /* Only ignore if activated via client area, not title bar */
                    if (g_titlebarclick) {
                        g_titlebarclick = 0;
                    } else {
                        g_ignoreclick = 1;
                    }
                }
            }
            break;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            update_display();
            EndPaint(hwnd, &ps);
            break;
        }

        case WM_SIZE:
            /* Handle video resize */
            if (g_videoPlaying) {
                char rcmd[128];
                RECT rrect;
                int rvideo_h;
                GetClientRect(g_hwnd, &rrect);
                rvideo_h = (rrect.bottom * TEXT_AREA_START) / SCREEN_HEIGHT;
                snprintf(rcmd, sizeof(rcmd), "put video destination at 0 0 %d %d", (int)rrect.right, rvideo_h);
                mciSendString(rcmd, NULL, 0, NULL);
            }
            break;

        case WM_CLOSE:
            g_running = 0;
            StopVideo();
            DestroyWindow(hwnd);
            break;

        case WM_DESTROY:
            PostQuitMessage(0);
            break;

        case MM_MCINOTIFY:
            /* Music finished playing - restart for looping */
            if (wParam == MCI_NOTIFY_SUCCESSFUL && g_mciDeviceID != 0) {
                RestartMusic();
            }
            break;

        case WM_TIMER:
            /* Poll music status for Win32s compatibility */
            if (wParam == MUSIC_TIMER_ID) {
                CheckMusicStatus();
            }
            break;

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

/* Main engine function */
static void run(void) {
    uint8_t bgpalette[32] = {0};
    FILE *script;
    FILE *config;
    long lineNumber = 0;
    char *line;
    char picture[260] = {0};
    char oldpicture[260] = {0};
    char musicfile[260] = {0};
    char oldmusicfile[260] = {0};
    int charlines = 0;
    int isplaying = 0;
    int willplaying = 0;
    int next;
    char *choicedata;
    char jumplabel[7] = {0};
    char savefile[14] = {0};
    long savepointer = 0;
    int savehistory[1000] = {0};
    int savehistory_idx = 0;
    long save_linenb = 0;
    int skipnexthistory = 0;
    int loadsave = 0;

    char spritefile[260] = {0};
    int posx = 0;
    int posy = 0;
    int isbackfunc = 0;
    char linex[4] = {0};
    char liney[4] = {0};

    reset_cursprites();
    reset_prevsprites();

    int spritecount = 0;
    char scriptfile[260] = "data\\stvn.vns";

    choicedata = (char *)malloc(11);
    if (choicedata == NULL) return;
    memset(choicedata, 0, 11);

    clear_screen();

    /* Parse config file */
    if (file_exists("stvn.ini") == 0) {
        config = fopen("stvn.ini", "r");
        line = get_line(config);

        while (line) {
            if (strlen(line) > 0) {
                if (*line == 'S') {
                    int filelength = (int)strlen(line) - 1;
                    if (filelength > 250) filelength = 250;
                    memset(scriptfile, 0, sizeof(scriptfile));
                    snprintf(scriptfile, sizeof(scriptfile), "data\\%.*s", filelength, line + 1);
                }
                if (*line == 'T') {
                    strncpy(g_windowTitle, line+1, sizeof(g_windowTitle) - 2);
                    g_windowTitle[sizeof(g_windowTitle) - 1] = '\0';
                    SetWindowTextA(g_hwnd, g_windowTitle);
                }
            }
            line = get_line(config);
        }
        fclose(config);
    } else {
        locate(0, 0);
        print_string("STVN.INI not found, using defaults:");
        locate(0, 16);
        print_string("Script file: DATA\\STVN.VNS");
        locate(0, 32);
        print_string("Press Space to continue...");
        update_display();

        while (read_keyboard_status() == 0 && g_running) {
            Sleep(5);
        }
    }

    script = fopen(scriptfile, "r");
    if (script == NULL) {
        clear_screen();
        locate(0, 0);
        print_string("Opening script failed: ");
        print_string(scriptfile);
        locate(0, 16);
        print_string("Press Space to quit...");
        update_display();

        while (read_keyboard_status() == 0 && g_running) {
            Sleep(5);
        }
        free(choicedata);
        return;
    }

    /* Main loop */
    while (g_running) {
        line = get_line(script);
        if (line == NULL) goto endprog;
        lineNumber++;

        if (strlen(line) > 0) {
            /* 'W': Wait for input */
            if (*line == 'W') {
                update_display();
                g_mouseclick = 0;  /* Clear any pending click */
                next = read_keyboard_status();
                while (next != 1 && !g_mouseclick && g_running) {
                    if (next == 2) goto endprog;

                    /* Save */
                    if (next == 3) {
                        SaveScreen();
                        DispLoadSave(1);

                        next = read_keyboard_status();
                        while (NoValidSaveChoice(next) && g_running) {
                            if (next == 7) RestoreWindowSize();
                            next = read_keyboard_status();
                            Sleep(5);
                        }

                        if (next != 2) {
                            HandleSaveFilename(next);
                            SaveMacro();
                        }
                        RestoreScreen();
                        update_display();
                    }

                    if (next == 8) {
                        SaveScreen();
                        DispLoadSave(2);
                        DeleteMacro();
                        RestoreScreen();
                        update_display();
                    }

                    /* Back */
                    if (next == 5 && savehistory_idx >= 2) {
                        save_linenb = savehistory[savehistory_idx - 2];
                        savehistory[savehistory_idx - 1] = 0;
                        savehistory_idx--;
                        skipnexthistory = 1;

                        memcpy(g_videoram + IMAGE_AREA_PIXELS, g_textarea, TEXT_AREA_PIXELS * sizeof(uint32_t));
                        locate(0, 337);
                        print_string(" Rolling back...");
                        RedrawBorder();
                        update_display();
                        goto seektoline;
                    }

                    /* Help */
                    if (next == 6) {
                        SaveScreen();
                        DispHelp();
                        while (next != 2 && g_running) {
                            if (next == 7) RestoreWindowSize();
                            next = read_keyboard_status();
                            Sleep(5);
                        }
                        RestoreScreen();
                        update_display();
                    }

                    /* Restore window size */
                    if (next == 7) RestoreWindowSize();

                    /* Load */
                    if (next == 4) {
                        loadsave = 1;
                        SaveScreen();
                        DispLoadSave(0);

                        while (NoValidSaveChoice(next) && g_running) {
                            if (next == 7) RestoreWindowSize();
                            next = read_keyboard_status();
                            Sleep(5);
                        }
                        RestoreScreen();

                    lblloadsave:
                        if (next != 2) {
                            HandleSaveFilename(next);

                            if (file_exists(savefile) == 0) {
                                memcpy(g_videoram + IMAGE_AREA_PIXELS, g_textarea, TEXT_AREA_PIXELS * sizeof(uint32_t));
                                locate(0, 337);
                                print_string(" Loading...");
                                RedrawBorder();
                                update_display();

                                FILE *savefp = fopen(savefile, "r");
                                char savestate[17] = {0};
                                char save_line[7] = {0};
                                char save_register[2] = {0};
                                save_linenb = 0;
                                fread(savestate, 1, 16, savefp);

                                memcpy(save_line, savestate, 6);
                                save_linenb = atoi(save_line);

                                for (int i = 0; i < 10; i++) {
                                    memcpy(save_register, savestate + 6 + i, 1);
                                    choicedata[i] = (char)atoi(save_register);
                                }

                                char histline[255] = {0};
                                if (fgets(histline, 255, savefp) != NULL && fgets(histline, 255, savefp) != NULL) {
                                    savehistory_idx = atoi(histline);
                                    if(savehistory_idx > 999) savehistory_idx=999;
                                    memset(savehistory, 0, sizeof(savehistory));
                                    for (int j = 0; j < savehistory_idx && j < 1000; j++) {
                                        if (fgets(histline, 255, savefp) == NULL) {
                                            savehistory_idx = j;
                                            break;
                                        }
                                        savehistory[j] = atoi(histline);
                                    }
                                }
                                fclose(savefp);
                                skipnexthistory = 1;

                            seektoline:
                                rewind(script);
                                lineNumber = 0;
                                savepointer = 0;
                                willplaying = 0;
                                spritecount = 0;

                                if (loadsave == 0) {
                                    backup_spritearray();
                                    reset_cursprites();
                                }

                                while (lineNumber < save_linenb) {
                                    line = get_line(script);
                                    if (line == NULL) goto endprog;
                                    lineNumber++;

                                    if (*line == 'I') {
                                        int filelen = (int)strlen(line) - 1;
                                        if (filelen > 250) filelen = 250;
                                        memset(picture, 0, sizeof(picture));
                                        snprintf(picture, sizeof(picture), "data\\%.*s", filelen, line + 1);
                                        reset_cursprites();
                                        spritecount = 0;
                                    }

                                    if (*line == 'R') {
                                        reset_cursprites();
                                        spritecount = 0;
                                    }

                                    if (*line == 'A') {
                                        if (strlen(line) >= 8 && spritecount < 256) {
                                            int filelen = (int)strlen(line) - 7;
                                            if (filelen > 250) filelen = 250;
                                            memset(currentsprites[spritecount].file, 0, sizeof(currentsprites[spritecount].file));
                                            memcpy(currentsprites[spritecount].file, line + 7, filelen);
                                            memset(linex, 0, 4);
                                            memset(liney, 0, 4);
                                            memcpy(linex, line + 1, 3);
                                            memcpy(liney, line + 4, 3);
                                            currentsprites[spritecount].x = atoi(linex);
                                            currentsprites[spritecount].y = atoi(liney);
                                            spritecount++;
                                        }
                                    }

                                    if (*line == 'P') {
                                        int filelen = (int)strlen(line) - 1;
                                        if (filelen > 0) {
                                            if (filelen > 250) filelen = 250;
                                            memset(musicfile, 0, sizeof(musicfile));
                                            snprintf(musicfile, sizeof(musicfile), "data\\%.*s", filelen, line + 1);
                                            willplaying = 1;
                                        }
                                    }

                                    if (*line == 'S') {
                                        savepointer = lineNumber;
                                    }

                                    if (*line == 'B') {
                                        if (strlen(line) == 8) {
                                            char lineregister[2] = {0};
                                            memcpy(lineregister, line + 1, 1);
                                            char selectedregister = (char)atoi(lineregister);

                                            char linechoice[2] = {0};
                                            memcpy(linechoice, line + 2, 1);
                                            char selectedchoice = (char)atoi(linechoice);
                                            if (selectedchoice > 3) selectedchoice = 3;

                                            if (choicedata[(int)selectedregister] == selectedchoice) {
                                                memcpy(jumplabel, line + 3, 6);
                                                while (1) {
                                                    line = get_line(script);
                                                    if (line == NULL) goto endprog;
                                                    lineNumber++;
                                                    if (strlen(line) >= 5) {
                                                        if (strncmp(jumplabel, line, 5) == 0) break;
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    if (*line == 'J') {
                                        if (strlen(line) >= 6) {
                                            memcpy(jumplabel, line + 1, 5);
                                            while (1) {
                                                line = get_line(script);
                                                if (line == NULL) goto endprog;
                                                lineNumber++;
                                                if (strlen(line) >= 5) {
                                                    if (strncmp(jumplabel, line, 5) == 0) break;
                                                }
                                            }
                                        }
                                    }

                                    if(*line == 'V') {
                                        ParseVCommand();
                                    }
                                }

                                /* Load background */
                                if (loadsave == 0) {
                                    if (strcmp(picture, oldpicture) != 0 || compare_sprites() != 0) {
                                        LoadBackgroundImage(picture, bgpalette, g_background);
                                        memcpy(oldpicture, picture, sizeof(oldpicture));
                                        RestoreScreen();
                                    }
                                } else {
                                    LoadBackgroundImage(picture, bgpalette, g_background);
                                    memcpy(oldpicture, picture, sizeof(oldpicture));
                                    RestoreScreen();
                                }

                                charlines = 0;

                                /* Display sprites */
                                if (compare_sprites() != 0 || loadsave == 1) {
                                    for (int sc = 0; sc < spritecount; sc++) {
                                        memset(spritefile, 0, sizeof(spritefile));
                                        snprintf(spritefile, sizeof(spritefile), "data\\%s", currentsprites[sc].file);
                                        DisplaySprite(spritefile, currentsprites[sc].x, currentsprites[sc].y);
                                    }
                                }

                                /* Play music if needed */
                                if (willplaying == 1) {
                                    if (strncmp(musicfile, oldmusicfile, sizeof(musicfile)) != 0) {
                                        if (isplaying) {
                                            StopMusic();
                                            isplaying = 0;
                                        }
                                        memcpy(oldmusicfile, musicfile, sizeof(oldmusicfile));
                                        PlayMusic(musicfile);
                                        isplaying = 1;
                                    }
                                }

                                loadsave = 0;
                                break;
                            }
                        }
                        RestoreScreen();
                        update_display();
                    }

                    g_mouseclick = 0;  /* Clear any clicks from dialogs */
                    next = read_keyboard_status();
                    Sleep(5);
                }
            }

            /* 'I': Change background image */
            if (*line == 'I') {
                int filelen = (int)strlen(line) - 1;
                if (filelen > 0) {
                    if (filelen > 250) filelen = 250;
                    memset(picture, 0, 260);
                    snprintf(picture, sizeof(picture), "data\\%.*s", filelen, line + 1);

                    if (LoadBackgroundImage(picture, bgpalette, g_background) == 0) {
                        memcpy(oldpicture, picture, sizeof(oldpicture));
                        RestoreScreen();
                    }
                    reset_cursprites();
                    spritecount = 0;
                    charlines = 0;
                }
            }

            /* 'R': Restore background */
            if (*line == 'R') {
                LoadBackgroundImage(picture, bgpalette, g_background);
                RestoreScreen();
                reset_cursprites();
                spritecount = 0;
            }

            /* 'S': Speaker change */
            if (*line == 'S') {
                charlines = 0;
                memcpy(g_videoram + IMAGE_AREA_PIXELS, g_textarea, TEXT_AREA_PIXELS * sizeof(uint32_t));
                RedrawBorder();

                locate(0, 322);
                print_string(line + 1);

                savepointer = lineNumber;
                if (skipnexthistory == 1) {
                    skipnexthistory = 0;
                } else {
                    if (savehistory_idx >= 1000) {
                        memmove(savehistory, savehistory + 1, 999 * sizeof(int));
                        savehistory[999] = 0;
                        savehistory_idx = 999;
                    }
                    savehistory[savehistory_idx++] = (int)lineNumber;
                }
            }

            /* 'E': Clear text area */
            if (*line == 'E') {
                charlines = 0;
                memcpy(g_videoram + IMAGE_AREA_PIXELS, g_textarea, TEXT_AREA_PIXELS * sizeof(uint32_t));
                RedrawBorder();
            }

            /* 'T': Text line */
            if (*line == 'T') {
                locate(0, 337 + charlines * 15);
                print_string(" ");
                print_string(line + 1);
                charlines++;
            }

            /* 'P': Play music */
            if (*line == 'P') {
                int filelen = (int)strlen(line) - 1;
                if (filelen > 0) {
                    if (filelen > 250) filelen = 250;
                    memset(musicfile, 0, sizeof(musicfile));
                    snprintf(musicfile, sizeof(musicfile), "data\\%.*s", filelen, line + 1);
                    if (strncmp(musicfile, oldmusicfile, sizeof(musicfile)) != 0) {
                        memcpy(oldmusicfile, musicfile, sizeof(oldmusicfile));
                        g_effectrunning = 1;
                        if (isplaying) {
                            StopMusic();
                            isplaying = 0;
                        }
                        PlayMusic(musicfile);
                        isplaying = 1;
                        FlushMessages();
                        g_effectrunning = 0;
                        g_lastkey = 0;
                    }
                }
            }

            /* 'M': Play video in image area */
            if (*line == 'M') {
                int filelen = (int)strlen(line) - 1;
                if (filelen > 0) {
                    char videofile[260];
                    int stopvideo = 0;
                    int rollbackvideo = 0;
                    MSG vmsg;
                    if (filelen > 250) filelen = 250;
                    snprintf(videofile, sizeof(videofile), "data\\%.*s", filelen, line + 1);
                    g_effectrunning = 1;
                    RedrawBorder();
                    update_display();

                    /* Stop music playing and invalidate oldmusicfile in case of rollback */
                    if (isplaying) {
                        StopMusic();
                        isplaying = 0;
                        memset(oldmusicfile, 0, sizeof(oldmusicfile));
                    }

                    PlayVideo(videofile);
                    /* Wait for video to finish or space to skip */
                    while (IsVideoPlaying() && g_running && !stopvideo) {
                        /* Process all messages, check for space key */
                        if (PeekMessage(&vmsg, NULL, 0, 0, PM_REMOVE)) {
                            if (vmsg.message == WM_QUIT) {
                                g_running = 0;
                            } else if (vmsg.message == WM_KEYDOWN && vmsg.wParam == VK_SPACE) {
                                stopvideo = 1;
                            } else if (vmsg.message == WM_KEYDOWN && vmsg.wParam == 'R') {
                                RestoreWindowSize();
                            } else if (vmsg.message == WM_KEYDOWN && vmsg.wParam == 'B') {
                                /* Only stop video for rollback if rollback is possible */
                                if (savehistory_idx >= 2) {
                                    stopvideo = 1;
                                    rollbackvideo = 1;
                                }
                            } else {
                                TranslateMessage(&vmsg);
                                DispatchMessage(&vmsg);
                            }
                        } else {
                            /* No messages - yield briefly */
                            Sleep(1);
                        }
                    }

                    StopVideo();
                    g_effectrunning = 0;
                    g_lastkey = 0;

                        printf("Rolling back to %d", savehistory[savehistory_idx - 2]);
                    /* Handle rollback if 'B' was pressed during video */
                    if (rollbackvideo && savehistory_idx >= 2) {
                        save_linenb = savehistory[savehistory_idx - 2];
                        savehistory[savehistory_idx - 1] = 0;
                        savehistory_idx--;
                        skipnexthistory = 1;
                        isbackfunc = 1;

                        memcpy(g_videoram + IMAGE_AREA_PIXELS, g_textarea, TEXT_AREA_PIXELS * sizeof(uint32_t));
                        locate(0, 337);
                        print_string(" Rolling back...");
                        RedrawBorder();
                        update_display();
                        goto seektoline;
                    }
                }
            }

            /* 'J': Jump to label */
            if (*line == 'J') {
                if (strlen(line) >= 6) {
                    memcpy(jumplabel, line + 1, 5);
                jumptolabel:
                    rewind(script);
                    lineNumber = 0;
                    while (1) {
                        line = get_line(script);
                        if (line == NULL) goto endprog;
                        lineNumber++;
                        if (strlen(line) >= 5) {
                            if (strncmp(jumplabel, line, 5) == 0) break;
                        }
                    }
                }
            }

            /* 'F': Jump to start */
            if (*line == 'F') {
                rewind(script);
                lineNumber = 0;
                savepointer = 0;
                savehistory_idx = 0;
                memset(savehistory, 0, sizeof(savehistory));
                willplaying = 0;
                spritecount = 0;
            }

            /* 'B': Conditional branch */
            if (*line == 'B') {
                if (strlen(line) == 8) {
                    char lineregister[2] = {0};
                    memcpy(lineregister, line + 1, 1);
                    char selectedregister = (char)atoi(lineregister);

                    char linechoice[2] = {0};
                    memcpy(linechoice, line + 2, 1);
                    char selectedchoice = (char)atoi(linechoice);
                    if (selectedchoice > 3) selectedchoice = 3;

                    if (choicedata[(int)selectedregister] == selectedchoice) {
                        memcpy(jumplabel, line + 3, 6);
                        goto jumptolabel;
                    }
                }
            }

            if(*line == 'V') {
                ParseVCommand();
            }

            /* 'C': Choice */
            if (*line == 'C') {
                if (strlen(line) == 3) {
                    char lineregister[2] = {0};
                    memcpy(lineregister, line + 1, 1);
                    char selectedregister = (char)atoi(lineregister);

                    char linechoice[2] = {0};
                    memcpy(linechoice, line + 2, 1);
                    char maxchoice = (char)atoi(linechoice);
                    if (maxchoice > 4) maxchoice = 4;
                    if (maxchoice < 2) maxchoice = 2;

                    update_display();
                    next = read_keyboard_status();
                    while (!(next >= 10 && next <= (9 + maxchoice)) && g_running) {
                        next = read_keyboard_status();
                        if (next == 2) goto endprog;
                        if (next == 7) RestoreWindowSize();

                        if (next == 3) {
                            SaveScreen();
                            DispLoadSave(1);

                            int savnext = read_keyboard_status();
                            while (NoValidSaveChoice(savnext) && g_running) {
                                if (savnext == 7) RestoreWindowSize();
                                savnext = read_keyboard_status();
                                Sleep(5);
                            }

                            if (savnext != 2) {
                                HandleSaveFilename(savnext);
                                SaveMacro();
                            }
                            next = 0;
                            RestoreScreen();
                            update_display();
                        }

                        if (next == 8) {
                            SaveScreen();
                            DispLoadSave(2);
                            DeleteMacro();
                            next = 0;
                            RestoreScreen();
                            update_display();
                        }

                        if (next == 4) {
                            SaveScreen();
                            DispLoadSave(0);

                            int ldnext = 0;
                            while (NoValidSaveChoice(ldnext) && g_running) {
                                if (ldnext == 7) RestoreWindowSize();
                                ldnext = read_keyboard_status();
                                Sleep(5);
                            }

                            HandleSaveFilename(ldnext);

                            RestoreScreen();
                            update_display();
                            if (file_exists(savefile) == 0) {
                                next = ldnext;
                                loadsave = 1;
                                goto lblloadsave;
                            }
                            next = 0;
                        }

                        if (next == 5 && savehistory_idx >= 2) {
                            save_linenb = savehistory[savehistory_idx - 2];
                            savehistory[savehistory_idx - 1] = 0;
                            savehistory_idx--;
                            skipnexthistory = 1;

                            memcpy(g_videoram + IMAGE_AREA_PIXELS, g_textarea, TEXT_AREA_PIXELS * sizeof(uint32_t));
                            locate(0, 337);
                            print_string(" Rolling back...");
                            RedrawBorder();
                            update_display();
                            goto seektoline;
                        }

                        if (next == 6) {
                            SaveScreen();
                            DispHelp();
                            while (next != 2 && g_running) {
                                if (next == 7) RestoreWindowSize();
                                next = read_keyboard_status();
                                Sleep(5);
                            }
                            RestoreScreen();
                            update_display();
                        }

                        Sleep(5);
                    }
                    choicedata[(int)selectedregister] = (char)(next - 9);
                }
            }

            /* 'D': Delay */
            if (*line == 'D') {
                if (strlen(line + 1) < 6) {
                    Sleep(atoi(line + 1) * 1000);
                }
            }

            /* 'X': Visual effect */
            if (*line == 'X') {
                if (strlen(line) >= 3) {
                    char effect[3] = {0};
                    memcpy(effect, line + 1, 2);
                    int effectnum = atoi(effect);

                    g_effectrunning = 1;

                    if (effectnum == 1) FxVWipeDown(COLOR_BLACK);
                    if (effectnum == 2) FxVWipeDown(COLOR_WHITE);
                    if (effectnum == 3) { FxVWipeDown(COLOR_BLACK); FxVWipeDown(COLOR_WHITE); }
                    if (effectnum == 4) { FxVWipeDown(COLOR_WHITE); FxVWipeDown(COLOR_BLACK); }
                    if (effectnum == 5) FxVWipeUp(COLOR_BLACK);
                    if (effectnum == 6) FxVWipeUp(COLOR_WHITE);
                    if (effectnum == 7) { FxVWipeUp(COLOR_BLACK); FxVWipeUp(COLOR_WHITE); }
                    if (effectnum == 8) { FxVWipeUp(COLOR_WHITE); FxVWipeUp(COLOR_BLACK); }
                    if (effectnum == 9) FxVWipeMidIn(COLOR_BLACK);
                    if (effectnum == 10) FxVWipeMidIn(COLOR_WHITE);
                    if (effectnum == 11) { FxVWipeMidIn(COLOR_BLACK); FxVWipeMidIn(COLOR_WHITE); }
                    if (effectnum == 12) { FxVWipeMidIn(COLOR_WHITE); FxVWipeMidIn(COLOR_BLACK); }
                    if (effectnum == 13) FxVWipeMidOut(COLOR_BLACK);
                    if (effectnum == 14) FxVWipeMidOut(COLOR_WHITE);
                    if (effectnum == 15) { FxVWipeMidOut(COLOR_BLACK); FxVWipeMidOut(COLOR_WHITE); }
                    if (effectnum == 16) { FxVWipeMidOut(COLOR_WHITE); FxVWipeMidOut(COLOR_BLACK); }
                    if (effectnum == 17) FxHWipeRight(COLOR_BLACK);
                    if (effectnum == 18) FxHWipeRight(COLOR_WHITE);
                    if (effectnum == 19) { FxHWipeRight(COLOR_BLACK); FxHWipeRight(COLOR_WHITE); }
                    if (effectnum == 20) { FxHWipeRight(COLOR_WHITE); FxHWipeRight(COLOR_BLACK); }
                    if (effectnum == 21) FxHWipeLeft(COLOR_BLACK);
                    if (effectnum == 22) FxHWipeLeft(COLOR_WHITE);
                    if (effectnum == 23) { FxHWipeLeft(COLOR_BLACK); FxHWipeLeft(COLOR_WHITE); }
                    if (effectnum == 24) { FxHWipeLeft(COLOR_WHITE); FxHWipeLeft(COLOR_BLACK); }
                    if (effectnum == 25) FxHWipeMidIn(COLOR_BLACK);
                    if (effectnum == 26) FxHWipeMidIn(COLOR_WHITE);
                    if (effectnum == 27) { FxHWipeMidIn(COLOR_BLACK); FxHWipeMidIn(COLOR_WHITE); }
                    if (effectnum == 28) { FxHWipeMidIn(COLOR_WHITE); FxHWipeMidIn(COLOR_BLACK); }
                    if (effectnum == 29) FxHWipeMidOut(COLOR_BLACK);
                    if (effectnum == 30) FxHWipeMidOut(COLOR_WHITE);
                    if (effectnum == 31) { FxHWipeMidOut(COLOR_BLACK); FxHWipeMidOut(COLOR_WHITE); }
                    if (effectnum == 32) { FxHWipeMidOut(COLOR_WHITE); FxHWipeMidOut(COLOR_BLACK); }
                    if (effectnum == 33) FxCircleOut(COLOR_BLACK);
                    if (effectnum == 34) FxCircleOut(COLOR_WHITE);
                    if (effectnum == 35) { FxCircleOut(COLOR_BLACK); FxCircleOut(COLOR_WHITE); }
                    if (effectnum == 36) { FxCircleOut(COLOR_WHITE); FxCircleOut(COLOR_BLACK); }
                    if (effectnum == 37) FxCircleIn(COLOR_BLACK);
                    if (effectnum == 38) FxCircleIn(COLOR_WHITE);
                    if (effectnum == 39) { FxCircleIn(COLOR_BLACK); FxCircleIn(COLOR_WHITE); }
                    if (effectnum == 40) { FxCircleIn(COLOR_WHITE); FxCircleIn(COLOR_BLACK); }

                    FlushMessages();
                    g_effectrunning = 0;
                    g_lastkey = 0;  /* Clear any key pressed during effect */
                }
            }

            /* 'A': Display sprite */
            if (*line == 'A') {
                if (strlen(line) >= 8 && spritecount < 256) {
                    int filelen = (int)strlen(line) - 7;
                    if (filelen > 250) filelen = 250;
                    memset(spritefile, 0, sizeof(spritefile));
                    snprintf(spritefile, sizeof(spritefile), "data\\%.*s", filelen, line + 7);
                    memset(linex, 0, 4);
                    memset(liney, 0, 4);
                    memcpy(linex, line + 1, 3);
                    memcpy(liney, line + 4, 3);
                    posx = atoi(linex);
                    posy = atoi(liney);
                    currentsprites[spritecount].x = posx;
                    currentsprites[spritecount].y = posy;
                    memcpy(currentsprites[spritecount].file, line + 7, filelen);
                    spritecount++;
                    DisplaySprite(spritefile, posx, posy);
                }
            }
        }

        RedrawBorder();
        update_display();
        Sleep(16); /* ~60 FPS */
    }

endprog:
    StopMusic();
    StopVideo();
    fclose(script);
    free(choicedata);
}

/* WinMain entry point */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    /* Register window class */
    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "STVNClass";
    g_hIcon = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(0), IMAGE_ICON, 32, 32, 0);
    wc.hIcon = g_hIcon;
//    wc.hIconSm = LoadIcon(NULL, IDI_APPLICATION);

    if (!RegisterClassEx(&wc)) {
        MessageBox(NULL, "Window Registration Failed!", "Error", MB_ICONEXCLAMATION | MB_OK);
        return 0;
    }

    /* Calculate window size for 640x400 client area */
    RECT rect = {0, 0, SCREEN_WIDTH, SCREEN_HEIGHT};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    /* Create window */
    g_hwnd = CreateWindowEx(
        0,
        "STVNClass",
        g_windowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        NULL, NULL, hInstance, NULL);

    if (g_hwnd == NULL) {
        MessageBox(NULL, "Window Creation Failed!", "Error", MB_ICONEXCLAMATION | MB_OK);
        return 0;
    }

/* if SCALED_RENDERING is defined, we will resize the window when starting */
#ifdef SCALED_RENDERING
    RestoreWindowSize();
#endif

    /* Allocate framebuffers (32-bit BGRA) */
    g_videoram = (uint32_t *)malloc(SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint32_t));
    g_background = (uint32_t *)malloc(IMAGE_AREA_PIXELS * sizeof(uint32_t));
    g_textarea = (uint32_t *)malloc(TEXT_AREA_PIXELS * sizeof(uint32_t));

    if (!g_videoram || !g_background || !g_textarea) {
        MessageBox(NULL, "Memory Allocation Failed!", "Error", MB_ICONEXCLAMATION | MB_OK);
        return 0;
    }

    /* Initialize to white */
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        g_videoram[i] = COLOR_WHITE;
    }
    for (int i = 0; i < IMAGE_AREA_PIXELS; i++) {
        g_background[i] = COLOR_WHITE;
    }
    for (int i = 0; i < TEXT_AREA_PIXELS; i++) {
        g_textarea[i] = COLOR_WHITE;
    }

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    /* Run the engine */
    run();

    /* Cleanup - destroy window first to prevent WM_PAINT accessing freed memory */
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
        g_hwnd = NULL;
    }

    free(g_videoram);
    g_videoram = NULL;
    free(g_background);
    g_background = NULL;
    free(g_textarea);
    g_textarea = NULL;

    if (g_hIcon) DestroyIcon(g_hIcon);
    UnregisterClass("STVNClass", hInstance);

    return 0;
}
