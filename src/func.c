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

#include "global.h"

/* Forward declarations */
static void update_display(void);
static void RestartMusic(void);
static void CheckMusicStatus(void);
static void ShowConfigDialog(void);
static int LoadBackgroundImage(const char *picture, uint8_t *bgpalette, uint32_t *background);

/* Configuration dialog control IDs */
#define IDC_VOLUME_LABEL    101
#define IDC_VOLUME_SLIDER   102
#define IDC_HQ_LABEL        103
#define IDC_HQ_CHECKBOX     104
#define IDC_DELAY_LABEL     105
#define IDC_DELAY_SLIDER    106

static HWND g_configDialog = NULL;
static int g_recenterDialog = 0;
static int g_repositionWindow = 0;
static DWORD g_lastrender = 0;
static DWORD g_renderthrottle = 15;

/* Wine workarounds */
#define IsWine() (GetProcAddress(GetModuleHandle("ntdll.dll"), "wine_get_version") != NULL)
static int g_dialogCreating = 0;
static int g_wineVolume = -1;

/* Process a message through IsDialogMessage for tab navigation,
 * but bypass it for keys that should close the dialog (c/q/escape) */
static int ConfigDialogMessage(MSG *msg) {
    if (!g_configDialog)
        return 0;
    if (msg->message == WM_KEYDOWN &&
        (msg->wParam == VK_ESCAPE || msg->wParam == 'C' || msg->wParam == 'Q')) {
        SendMessage(g_configDialog, WM_CLOSE, 0, 0);
        return 1;
    }
    return IsDialogMessage(g_configDialog, msg);
}

static void RecenterConfigDialog(void) {
    RECT clientRect, dlgRect;
    POINT topLeft = {0, 0};
    int x, y;
    GetClientRect(g_hwnd, &clientRect);
    ClientToScreen(g_hwnd, &topLeft);
    GetWindowRect(g_configDialog, &dlgRect);
    x = topLeft.x + ((clientRect.right - clientRect.left) - (dlgRect.right - dlgRect.left)) / 2;
    y = topLeft.y + ((clientRect.bottom - clientRect.top) - (dlgRect.bottom - dlgRect.top)) / 2;
    SetWindowPos(g_configDialog, HWND_TOP, x, y, 0, 0, SWP_NOSIZE);
}

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

    /* Open the audio file */
    memset(&mciOpen, 0, sizeof(mciOpen));
    mciOpen.lpstrElementName = fullpath;
    mciOpen.lpstrAlias = "w3vn_music";

    if (IsWine()) {
        /* Wine: force mpegvideo so setaudio volume works */
        mciOpen.lpstrDeviceType = "mpegvideo";
        dwReturn = mciSendCommand(0, MCI_OPEN, MCI_OPEN_ELEMENT | MCI_OPEN_ALIAS | MCI_OPEN_TYPE, (DWORD)(LPVOID)&mciOpen);
    } else {
        /* Windows: let MCI auto-detect the type */
        dwReturn = mciSendCommand(0, MCI_OPEN, MCI_OPEN_ELEMENT | MCI_OPEN_ALIAS, (DWORD)(LPVOID)&mciOpen);
    }
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

    /* Wine: apply cached volume to the newly opened MCI device */
    if (g_wineVolume >= 0) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "setaudio w3vn_music volume to %d", (g_wineVolume * 1000) / 100);
        mciSendString(cmd, NULL, 0, NULL);
    }

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
/* Calculate video window position/size matching update_display layout */
static void CalcVideoWindowRect(RECT *out, int video_w, int video_h) {
    RECT rect;
    int content_w, content_x;
    int text_h = SCREEN_HEIGHT - TEXT_AREA_START;
    int image_h = TEXT_AREA_START;
    int win_x, win_y, win_w, win_h;

    GetClientRect(g_hwnd, &rect);

    /* Calculate content area the same way update_display does */
    if (SCREEN_WIDTH * rect.bottom > SCREEN_HEIGHT * rect.right) {
        content_w = rect.right;
    } else {
        content_w = (SCREEN_WIDTH * rect.bottom) / SCREEN_HEIGHT;
    }
    content_x = (rect.right - content_w) / 2;

    /* Calculate image area position */
    int text_dest_h = (text_h * content_w) / SCREEN_WIDTH;
    int text_dest_y = rect.bottom - text_dest_h;
    int image_dest_h = (image_h * content_w) / SCREEN_WIDTH;
    int available_h = text_dest_y;
    int padding = available_h - image_dest_h;
    int image_dest_y = padding / 2;

    /* Calculate video position within image area */
    if (video_w > 0 && video_h > 0) {
        if (video_w * image_dest_h > video_h * content_w) {
            win_w = content_w;
            win_h = (video_h * content_w) / video_w;
        } else {
            win_h = image_dest_h;
            win_w = (video_w * image_dest_h) / video_h;
        }
        win_x = content_x + (content_w - win_w) / 2;
        win_y = image_dest_y + (image_dest_h - win_h) / 2;
    } else {
        win_x = content_x;
        win_y = image_dest_y;
        win_w = content_w;
        win_h = image_dest_h;
    }

    out->left = win_x;
    out->top = win_y;
    out->right = win_w;
    out->bottom = win_h;
}

static void PlayVideo(const char *filename) {
    char cmd[512];
    char result[128];
    int video_w = 0, video_h = 0;
    RECT vwrect;

    /* Stop any currently playing video */
    if (g_videoPlaying) {
        mciSendString("stop video", NULL, 0, NULL);
        mciSendString("close video", NULL, 0, NULL);
        g_videoPlaying = 0;
    }
    if (g_videoWindow) {
        DestroyWindow(g_videoWindow);
        g_videoWindow = NULL;
    }

    /* Open the video file */
    snprintf(cmd, sizeof(cmd), "open \"%s\" alias video", filename);
    if (mciSendString(cmd, NULL, 0, NULL) != 0) {
        return;
    }

    /* Fill image area with black for letterboxing */
    int i;
    int image_pixels = SCREEN_WIDTH * TEXT_AREA_START;
    for (i = 0; i < image_pixels; i++) {
        g_videoram[i] = COLOR_BLACK;
    }
    update_display();

    /* Get video native dimensions */
    if (mciSendString("where video source", result, sizeof(result), NULL) == 0) {
        /* Result format: "x y width height" */
        sscanf(result, "%*d %*d %d %d", &video_w, &video_h);
    }
    /* Store for resize handler */
    g_videoWidth = video_w;
    g_videoHeight = video_h;

    /* Calculate child window position/size */
    CalcVideoWindowRect(&vwrect, video_w, video_h);

    /* Create child window for video (Wine workaround) */
    g_videoWindow = CreateWindowEx(
        0, "STVNVideoClass", NULL,
        WS_CHILD | WS_VISIBLE,
        vwrect.left, vwrect.top, vwrect.right, vwrect.bottom,
        g_hwnd, NULL, GetModuleHandle(NULL), NULL);

    /* Tell MCI to use the child window for video display */
    snprintf(cmd, sizeof(cmd), "window video handle %lu", (unsigned long)(uintptr_t)g_videoWindow);
    mciSendString(cmd, NULL, 0, NULL);

    /* Video fills the entire child window */
    snprintf(cmd, sizeof(cmd), "put video destination at 0 0 %d %d", vwrect.right, vwrect.bottom);
    mciSendString(cmd, NULL, 0, NULL);

    /* Start playback */
    mciSendString("play video", NULL, 0, NULL);
    g_videoPlaying = 1;

    /* Wine: apply cached volume to video device */
    if (g_wineVolume >= 0) {
        char vcmd[64];
        snprintf(vcmd, sizeof(vcmd), "setaudio video volume to %d", (g_wineVolume * 1000) / 100);
        mciSendString(vcmd, NULL, 0, NULL);
    }

    /* Restore focus to main window */
    SetFocus(g_hwnd);
}

/* Stop currently playing video */
static void StopVideo(void) {
    /* Always try to stop/close, even if g_videoPlaying is 0 (state may be out of sync) */
    mciSendString("stop video wait", NULL, 0, NULL);
    mciSendString("close video wait", NULL, 0, NULL);
    g_videoPlaying = 0;
    g_videoWidth = 0;
    g_videoHeight = 0;
    /* Destroy video child window */
    if (g_videoWindow) {
        DestroyWindow(g_videoWindow);
        g_videoWindow = NULL;
    }
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

    /* Right clipping: respect text box right border (x=639) or screen edge */
    int right_limit = (py >= TEXT_AREA_START) ? SCREEN_WIDTH - 1 : SCREEN_WIDTH;

    if (px >= right_limit || py >= SCREEN_HEIGHT) return;

    for (int row = 0; row < 15; row++) {
        int screen_row = py + row;
        if (screen_row >= SCREEN_HEIGHT) break;

        uint8_t glyph_row = g_font8x15[idx][row];
        uint32_t *pixel = g_videoram + screen_row * SCREEN_WIDTH + px;

        /* Write pixels for this row of the glyph, clipping at right boundary */
        for (int bit = 0; bit < 8 && px + bit < right_limit; bit++) {
            if (glyph_row & (0x80 >> bit)) {
                pixel[bit] = COLOR_BLACK;
            } else {
                pixel[bit] = COLOR_WHITE;
            }
        }
    }
    g_cursorX += 8;
}

void CALLBACK Timer0Proc(HWND hWnd, unsigned int msg, unsigned int idTimer, DWORD dwTime)
{
    DWORD now = timeGetTime();
    if ((now - g_lastrender) >= g_renderthrottle) {
        update_display();
    }
}

/* Print a string with optional per-character delay.
 * If a key is pressed during the delay, sets g_textskip to skip the rest
 * of the current text block (all consecutive 'T' lines). */
static void print_string(const char *str) {
    DWORD string_start = timeGetTime();
    int char_count = 0;
    if(g_textdelay > 0 && g_textskip > 0) SetTimer(g_hwnd, DEFER_RENDER_TIME_ID, 15, (TIMERPROC) Timer0Proc);
    while (*str) {
        if (*str == '\n') {
            g_cursorX = 0;
            /* Text area uses 15-pixel line height, image area uses 16 */
            g_cursorY += (g_cursorY >= TEXT_AREA_START) ? 15 : 16;
        } else if (*str == '\r') {
            g_cursorX = 0;
        } else {
            print_char(*str);
            char_count++;
            if (g_textdelay > 0 && g_textskip > 0) {
                DWORD target = string_start + char_count * (DWORD)g_textdelay;
                while ((int)(target - timeGetTime()) > 0) {
                    MSG msg;
                    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                        if (msg.message == WM_QUIT) {
                            g_running = 0;
                            KillTimer(g_hwnd, DEFER_RENDER_TIME_ID);
                            return;
                        }
                        if (ConfigDialogMessage(&msg))
                            continue;
                        TranslateMessage(&msg);
                        DispatchMessage(&msg);
                    }
                    if (g_lastkey) {
                        g_lastkey = 0;
                        g_textskip = -1;
                        break;
                    }
                }
            }
        }
        str++;
    }
    KillTimer(g_hwnd, DEFER_RENDER_TIME_ID);
}

/* Clear the entire screen to white */
static void clear_screen(void) {
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        g_videoram[i] = COLOR_WHITE;
    }
    g_cursorX = 0;
    g_cursorY = 0;
}

/* HQ2x scaling functions: hybrid filtering with bilinear for image/text content
   and nearest-neighbor for the text area border */

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

    /* Destination X boundaries for left/right borders.
       Use ceiling to ensure NN is only for pixels that actually sample from border,
       not text content. ceil(a/b) = (a + b - 1) / b */
    int left_end = (v_border_width * dst_w + src_w - 1) / src_w;
    int right_start = ((src_w - v_border_width) * dst_w + src_w - 1) / src_w;

    /* Source row boundaries for text area (in flipped buffer) */
    int src_bot_border_end = h_border_margin + 1;        /* First text row: 3 */
    int src_top_border_start = text_top_flipped - h_border_margin; /* Last text row + 1: 77 */

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

        /* Decide based on which source rows are actually being sampled */
        int both_in_bot_border = (y0 < src_bot_border_end && y1 < src_bot_border_end);
        int both_in_top_border = (y0 >= src_top_border_start && y1 >= src_top_border_start &&
                                  y0 < src_h - TEXT_AREA_START && y1 < src_h - TEXT_AREA_START);
        int both_in_text = (y0 >= src_bot_border_end && y1 < src_top_border_start);
        int both_in_image = (y0 >= src_h - TEXT_AREA_START);

        if (both_in_bot_border || both_in_top_border) {
            /* Both source rows in border: all nearest-neighbor */
            nn_row(row_nn, out, 0, dst_w, x_ratio_nn);
        } else if (both_in_image) {
            /* Image area: all bilinear */
            bilinear_row(row0, row1, out, 0, dst_w, src_w, x_ratio_bl, fy);
        } else if (both_in_text) {
            /* Text content rows: NN for left/right borders, bilinear for middle */
            nn_row(row_nn, out, 0, left_end, x_ratio_nn);
            bilinear_row(row0, row1, out, left_end, right_start, src_w, x_ratio_bl, fy);
            nn_row(row_nn, out, right_start, dst_w, x_ratio_nn);
        } else if (y0 < src_h - TEXT_AREA_START && y1 >= src_h - TEXT_AREA_START) {
            /* Transition row straddling top border / image boundary */
            if (src_y_nn >= src_h - TEXT_AREA_START) {
                /* NN maps to image: bilinear but clamp lower row to image area */
                uint32_t *clamped_row0 = src + (src_h - TEXT_AREA_START) * src_w;
                bilinear_row(clamped_row0, row1, out, 0, dst_w, src_w, x_ratio_bl, fy);
            } else {
                /* NN maps to border: nearest-neighbor */
                nn_row(row_nn, out, 0, dst_w, x_ratio_nn);
            }
        } else {
            /* Other transition rows (within text box): per-pixel hybrid */
            hybrid_row(row_nn, row0, row1, out, dst_w, src_w,
                       x_ratio_nn, x_ratio_bl, fy, left_end, right_start);
        }
    }
}

/* Update the Windows display from our framebuffer */
static void update_display(void) {
    if (!g_hwnd || !g_videoram) return;

    RECT rect;
    GetClientRect(g_hwnd, &rect);
    int win_w = rect.right;
    int win_h = rect.bottom;

    if (win_w <= 0 || win_h <= 0) return;

    int text_h = SCREEN_HEIGHT - TEXT_AREA_START;  /* 80 */
    int image_h = TEXT_AREA_START;                  /* 320 */

    /* Calculate width preserving aspect ratio */
    int dest_x, dest_w;
    if (SCREEN_WIDTH * win_h > SCREEN_HEIGHT * win_w) {
        /* Window is taller - use full width */
        dest_w = win_w;
    } else {
        /* Window is wider - width based on height */
        dest_w = (SCREEN_WIDTH * win_h) / SCREEN_HEIGHT;
    }
    dest_x = (win_w - dest_w) / 2;

    /* Textbox at bottom, scaled proportionally */
    int text_scaled_h = (text_h * dest_w) / SCREEN_WIDTH;
    int text_dest_y = win_h - text_scaled_h;

    /* Image above textbox, with padding split top and middle */
    int image_scaled_h = (image_h * dest_w) / SCREEN_WIDTH;
    int available_h = text_dest_y;
    int padding = available_h - image_scaled_h;
    int image_dest_y = padding / 2;

    /* Flip source for bottom-up DIB format */
    uint32_t *flipped = (uint32_t *)malloc(SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint32_t));
    if (!flipped) return;

    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        memcpy(flipped + y * SCREEN_WIDTH,
               g_videoram + (SCREEN_HEIGHT - 1 - y) * SCREEN_WIDTH,
               SCREEN_WIDTH * sizeof(uint32_t));
    }

    if (g_hq2x) {
        /* HQ2x: hybrid scaling with bilinear for image, nearest-neighbor for text borders */
        int content_h = image_scaled_h + text_scaled_h;
        if (dest_w <= 0 || content_h <= 0) {
            free(flipped);
            return;
        }

        uint32_t *scaled = (uint32_t *)malloc(dest_w * content_h * sizeof(uint32_t));
        if (!scaled) {
            free(flipped);
            return;
        }

        hybrid_scale(flipped, SCREEN_WIDTH, SCREEN_HEIGHT, scaled, dest_w, content_h);

        /* hybrid_scale maps source row text_h to a destination row that may not
           equal text_scaled_h due to integer division rounding.  Compute the
           actual split so the SetDIBitsToDevice calls match the buffer content. */
        int buf_text_h = (text_h * content_h + SCREEN_HEIGHT - 1) / SCREEN_HEIGHT;
        int buf_image_h = content_h - buf_text_h;
        int hq_text_dest_y = win_h - buf_text_h;
        int hq_image_dest_y = padding / 2;

        BITMAPINFOHEADER bmi;
        memset(&bmi, 0, sizeof(bmi));
        bmi.biSize = sizeof(BITMAPINFOHEADER);
        bmi.biWidth = dest_w;
        bmi.biHeight = content_h;
        bmi.biPlanes = 1;
        bmi.biBitCount = 32;
        bmi.biCompression = BI_RGB;
        bmi.biSizeImage = 0;

        HDC hdc = GetDC(g_hwnd);
        if (hdc) {
            HBRUSH blackBrush = (HBRUSH)GetStockObject(BLACK_BRUSH);
            RECT bar;
            if (dest_x > 0) {
                SetRect(&bar, 0, 0, dest_x, win_h);
                FillRect(hdc, &bar, blackBrush);
                SetRect(&bar, dest_x + dest_w, 0, win_w, win_h);
                FillRect(hdc, &bar, blackBrush);
            }
            if (padding > 0) {
                SetRect(&bar, dest_x, 0, dest_x + dest_w, hq_image_dest_y);
                FillRect(hdc, &bar, blackBrush);
                SetRect(&bar, dest_x, hq_image_dest_y + buf_image_h, dest_x + dest_w, hq_text_dest_y);
                FillRect(hdc, &bar, blackBrush);
            }

            SetDIBitsToDevice(hdc, dest_x, hq_image_dest_y, dest_w, buf_image_h,
                              0, buf_text_h, 0, content_h,
                              scaled, (BITMAPINFO *)&bmi, DIB_RGB_COLORS);
            SetDIBitsToDevice(hdc, dest_x, hq_text_dest_y, dest_w, buf_text_h,
                              0, 0, 0, content_h,
                              scaled, (BITMAPINFO *)&bmi, DIB_RGB_COLORS);

            ReleaseDC(g_hwnd, hdc);
        }

        free(scaled);
    } else {
        /* Standard rendering: nearest-neighbor via StretchDIBits */
        BITMAPINFOHEADER bmi;
        memset(&bmi, 0, sizeof(bmi));
        bmi.biSize = sizeof(BITMAPINFOHEADER);
        bmi.biWidth = SCREEN_WIDTH;
        bmi.biHeight = SCREEN_HEIGHT;
        bmi.biPlanes = 1;
        bmi.biBitCount = 32;
        bmi.biCompression = BI_RGB;
        bmi.biSizeImage = 0;

        HDC hdc = GetDC(g_hwnd);
        if (hdc) {
            HBRUSH blackBrush = (HBRUSH)GetStockObject(BLACK_BRUSH);
            RECT bar;
            if (dest_x > 0) {
                SetRect(&bar, 0, 0, dest_x, win_h);
                FillRect(hdc, &bar, blackBrush);
                SetRect(&bar, dest_x + dest_w, 0, win_w, win_h);
                FillRect(hdc, &bar, blackBrush);
            }
            if (padding > 0) {
                SetRect(&bar, dest_x, 0, dest_x + dest_w, image_dest_y);
                FillRect(hdc, &bar, blackBrush);
                SetRect(&bar, dest_x, image_dest_y + image_scaled_h, dest_x + dest_w, text_dest_y);
                FillRect(hdc, &bar, blackBrush);
            }

            StretchDIBits(hdc, dest_x, image_dest_y, dest_w, image_scaled_h,
                          0, text_h, SCREEN_WIDTH, image_h,
                          flipped, (BITMAPINFO *)&bmi, DIB_RGB_COLORS, SRCCOPY);
            StretchDIBits(hdc, dest_x, text_dest_y, dest_w, text_scaled_h,
                          0, 0, SCREEN_WIDTH, text_h,
                          flipped, (BITMAPINFO *)&bmi, DIB_RGB_COLORS, SRCCOPY);

            ReleaseDC(g_hwnd, hdc);
        }
    }

    free(flipped);
    g_lastrender = timeGetTime();
}

/* Center the window on screen */
static void CenterWindow(void) {
    RECT rect;
    GetWindowRect(g_hwnd, &rect);
    int w = rect.right - rect.left;
    int h = rect.bottom - rect.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    SetWindowPos(g_hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

static void RepositionWindow(void) {
    RECT wrect;
    GetWindowRect(g_hwnd, &wrect);
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    int new_x = wrect.left;
    int new_y = wrect.top;

    /* Adjust X position if needed */
    if (wrect.left < 0) {
        new_x = 0;
    } else if (wrect.right > screen_w) {
        new_x = screen_w - (wrect.right - wrect.left);
    }

    /* Adjust Y position if needed */
    if (wrect.top < 0) {
        new_y = 0;
    } else if (wrect.bottom > screen_h) {
        new_y = screen_h - (wrect.bottom - wrect.top);
    }

    /* Only move if adjustment is needed */
    if (new_x != wrect.left || new_y != wrect.top) {
        SetWindowPos(g_hwnd, NULL, new_x, new_y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
}

/* Restore window size (1280x800 if HQ2x enabled, 640x400 otherwise) */
static void RestoreWindowSize(void) {
    RECT rect;
    if (g_hq2x) {
        rect.left = 0; rect.top = 0;
        rect.right = 1280; rect.bottom = 800;
    } else {
        rect.left = 0; rect.top = 0;
        rect.right = SCREEN_WIDTH; rect.bottom = SCREEN_HEIGHT;
    }
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(g_hwnd, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER);

    if (g_configDialog) {
        if (IsWine()) {
            /* Wine: defer re-center since the WM processes the resize
               asynchronously; the dialog's 100ms timer handles it */
            g_recenterDialog = 1;
            SetWindowPos(g_configDialog, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        } else {
            /* Windows: re-center immediately */
            RecenterConfigDialog();
        }
    }
}

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
        if (ConfigDialogMessage(&msg))
            continue;
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
    /* Dialog 142x162 centered in 640x320 image area */
    /* Interior 140x160 (17 chars * 8px + 2px padding each side) */
    /* Border wraps outside: x 249..390, y 79..240 */
    for (int y = 80; y <= 239; y++) {
        for (int x = 250; x <= 389; x++) {
            g_videoram[y * SCREEN_WIDTH + x] = COLOR_WHITE;
        }
    }

    locate(252, 82);
    print_string("-     Usage     -");
    locate(252, 98);
    print_string("[q] Quit         ");
    locate(252, 114);
    print_string("[b] Back         ");
    locate(252, 130);
    print_string("[l] Load save    ");
    locate(252, 146);
    print_string("[s] Save state   ");
    locate(252, 162);
    print_string("[e] Erase save   ");
    locate(252, 178);
    print_string("[r] Restore size ");
    locate(252, 194);
    print_string("[c] Config       ");
    locate(252, 210);
    print_string("[ ] Advance      ");
    locate(252, 226);
    print_string("[esc] Restart    ");

    DrawHLine(249, 79, 390);
    DrawHLine(249, 240, 390);
    DrawVLine(249, 79, 240);
    DrawVLine(390, 79, 240);

    update_display();
}

static void DispQuit(void) {
    /* Dialog 116x33 centered in 640x320 image area */
    /* x: (640-116)/2 = 262, y: (320-33)/2 = 144 */
    for (int y = 144; y <= 175; y++) {
        for (int x = 262; x <= 377; x++) {
            g_videoram[y * SCREEN_WIDTH + x] = COLOR_WHITE;
        }
    }

    locate(264, 146);
    print_string("-    Quit    -");
    locate(264, 162);
    print_string("[1] Yes [2] No");

    DrawHLine(262, 144, 377);
    DrawHLine(262, 176, 377);
    DrawVLine(262, 144, 176);
    DrawVLine(377, 144, 176);

    update_display();
}

static void DispEsc(void) {
    /* Dialog 142x34 centered in 640x320 image area */
    /* Interior 140x32 (17 chars * 8px + 2px padding each side) */
    /* Border wraps outside: x 249..390, y 143..176 */
    for (int y = 144; y <= 175; y++) {
        for (int x = 250; x <= 389; x++) {
            g_videoram[y * SCREEN_WIDTH + x] = COLOR_WHITE;
        }
    }

    locate(252, 146);
    print_string("-    Restart    -");
    locate(252, 162);
    print_string(" [1] Yes  [2] No ");

    DrawHLine(249, 143, 390);
    DrawHLine(249, 176, 390);
    DrawVLine(249, 143, 176);
    DrawVLine(390, 143, 176);

    update_display();
}

/* Delay function compatible with Win32s
 * Uses timeGetTime() from winmm.dll for ~1ms resolution */
static void FxDelay(DWORD ms) {
    DWORD start, elapsed;
    MSG msg;

    start = timeGetTime();

    while (g_running) {
        /* Pump messages to keep window responsive */
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (ConfigDialogMessage(&msg))
                continue;
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
    SetTimer(g_hwnd, DEFER_RENDER_TIME_ID, 15, (TIMERPROC) Timer0Proc);
    for (int i = 0; i < 320; i += 8) {
        FillRows(i, 8, color);
        FxDelay(15);
        if (!g_running) break;
    }
    KillTimer(g_hwnd, DEFER_RENDER_TIME_ID);
    if (g_running) update_display();
}

static void FxVWipeUp(uint32_t color) {
    SetTimer(g_hwnd, DEFER_RENDER_TIME_ID, 15, (TIMERPROC) Timer0Proc);
    for (int i = 312; i >= 0; i -= 8) {
        FillRows(i, 8, color);
        FxDelay(15);
        if (!g_running) break;
    }
    KillTimer(g_hwnd, DEFER_RENDER_TIME_ID);
    if (g_running) update_display();
}

static void FxVWipeMidIn(uint32_t color) {
    /* Wipe from edges (0 and 319) toward center (160) */
    SetTimer(g_hwnd, DEFER_RENDER_TIME_ID, 15, (TIMERPROC) Timer0Proc);
    for (int i = 0; i < 160; i += 8) {
        FillRows(i, 8, color);                /* Top edge moving down */
        FillRows(312 - i, 8, color);          /* Bottom edge moving up */
        FxDelay(15);
        if (!g_running) break;
    }
    KillTimer(g_hwnd, DEFER_RENDER_TIME_ID);
    if (g_running) update_display();
}

static void FxVWipeMidOut(uint32_t color) {
    /* Wipe from center (160) toward edges */
    SetTimer(g_hwnd, DEFER_RENDER_TIME_ID, 15, (TIMERPROC) Timer0Proc);
    for (int i = 0; i < 160; i += 8) {
        FillRows(160 + i, 8, color);          /* Center moving down */
        FillRows(152 - i, 8, color);          /* Center moving up */
        FxDelay(15);
        if (!g_running) break;
    }
    KillTimer(g_hwnd, DEFER_RENDER_TIME_ID);
    if (g_running) update_display();
}

static void FxHWipeRight(uint32_t color) {
    /* Wipe 32 pixels at a time for speed */
    SetTimer(g_hwnd, DEFER_RENDER_TIME_ID, 15, (TIMERPROC) Timer0Proc);
    for (int col = 0; col < SCREEN_WIDTH; col += 32) {
        for (int line = 0; line < 320; line++) {
            uint32_t *row = g_videoram + line * SCREEN_WIDTH + col;
            for (int p = 0; p < 32 && col + p < SCREEN_WIDTH; p++) {
                row[p] = color;
            }
        }
        FxDelay(15);
        if (!g_running) break;
    }
    KillTimer(g_hwnd, DEFER_RENDER_TIME_ID);
    if (g_running) update_display();
}

static void FxHWipeLeft(uint32_t color) {
    /* Wipe 32 pixels at a time for speed */
    SetTimer(g_hwnd, DEFER_RENDER_TIME_ID, 15, (TIMERPROC) Timer0Proc);
    for (int col = SCREEN_WIDTH - 32; col >= 0; col -= 32) {
        for (int line = 0; line < 320; line++) {
            uint32_t *row = g_videoram + line * SCREEN_WIDTH + col;
            for (int p = 0; p < 32; p++) {
                row[p] = color;
            }
        }
        FxDelay(15);
        if (!g_running) break;
    }
    KillTimer(g_hwnd, DEFER_RENDER_TIME_ID);
    if (g_running) update_display();
}

static void FxHWipeMidIn(uint32_t color) {
    /* 64 pixels at a time (32 from each side) */
    SetTimer(g_hwnd, DEFER_RENDER_TIME_ID, 15, (TIMERPROC) Timer0Proc);
    for (int col = 0; col < SCREEN_WIDTH / 2; col += 32) {
        for (int line = 0; line < 320; line++) {
            uint32_t *row = g_videoram + line * SCREEN_WIDTH;
            for (int p = 0; p < 32 && col + p < SCREEN_WIDTH / 2; p++) {
                row[col + p] = color;
                row[SCREEN_WIDTH - 1 - col - p] = color;
            }
        }
        FxDelay(15);
        if (!g_running) break;
    }
    KillTimer(g_hwnd, DEFER_RENDER_TIME_ID);
    if (g_running) update_display();
}

static void FxHWipeMidOut(uint32_t color) {
    /* 64 pixels at a time (32 from center to each side) */
    SetTimer(g_hwnd, DEFER_RENDER_TIME_ID, 15, (TIMERPROC) Timer0Proc);
    for (int col = 0; col < SCREEN_WIDTH / 2; col += 32) {
        for (int line = 0; line < 320; line++) {
            uint32_t *row = g_videoram + line * SCREEN_WIDTH;
            for (int p = 0; p < 32 && col + p < SCREEN_WIDTH / 2; p++) {
                row[SCREEN_WIDTH / 2 - 1 - col - p] = color;
                row[SCREEN_WIDTH / 2 + col + p] = color;
            }
        }
        FxDelay(15);
        if (!g_running) break;
    }
    KillTimer(g_hwnd, DEFER_RENDER_TIME_ID);
    if (g_running) update_display();
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

    SetTimer(g_hwnd, DEFER_RENDER_TIME_ID, 15, (TIMERPROC) Timer0Proc);
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
        FxDelay(40);
        if (!g_running) break;
    }
    KillTimer(g_hwnd, DEFER_RENDER_TIME_ID);
    if (g_running) update_display();
}

static void FxCircleIn(uint32_t color) {
    int bcx = 20;
    int bcy = 5;

    SetTimer(g_hwnd, DEFER_RENDER_TIME_ID, 15, (TIMERPROC) Timer0Proc);
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
        FxDelay(40);
        if (!g_running) break;
    }
    KillTimer(g_hwnd, DEFER_RENDER_TIME_ID);
    if (g_running) update_display();
}

/* Fade the image area to black in 20 steps over 2 seconds */
static void FxFadeOut(void) {
    const int steps = 20;
    uint32_t *original = (uint32_t *)malloc(IMAGE_AREA_PIXELS * sizeof(uint32_t));
    if (!original) return;

    memcpy(original, g_videoram, IMAGE_AREA_PIXELS * sizeof(uint32_t));

    SetTimer(g_hwnd, DEFER_RENDER_TIME_ID, 15, (TIMERPROC) Timer0Proc);

    /* Fade steps: blend original pixels towards black */
    for (int step = 1; step <= steps; step++) {
        uint8_t lut[256];
        int inv = steps - step;
        for (int i = 0; i < 256; i++)
            lut[i] = i * inv / steps;

        uint32_t *src = original;
        uint32_t *dst = g_videoram;
        uint32_t *dst_end = g_videoram + IMAGE_AREA_PIXELS;
        for (; dst < dst_end; src++, dst++) {
            uint32_t pixel = *src;
            *dst = (pixel & 0xFF000000)
                 | ((uint32_t)lut[(pixel >> 16) & 0xFF] << 16)
                 | ((uint32_t)lut[(pixel >> 8) & 0xFF] << 8)
                 | lut[pixel & 0xFF];
        }

        FxDelay(50);
        if (!g_running) break;
    }

    KillTimer(g_hwnd, DEFER_RENDER_TIME_ID);
    free(original);

    /* Ensure final state is pure black */
    if (g_running) {
        for (uint32_t *ptr = g_videoram; ptr < g_videoram + IMAGE_AREA_PIXELS; ptr++)
            *ptr = COLOR_BLACK;
        update_display();
    }
}

/* Fade from black to an image in 20 steps over 2 seconds */
static void FxFadeIn(const char *filename) {
    uint32_t *target_buffer = (uint32_t *)malloc(IMAGE_AREA_PIXELS * sizeof(uint32_t));
    if (!target_buffer) return;

    /* Load the target image */
    uint8_t temp_palette[32];
    if (LoadBackgroundImage(filename, temp_palette, target_buffer) != 0) {
        free(target_buffer);
        return;
    }

    /* Start with black screen */
    for (uint32_t *ptr = g_videoram; ptr < g_videoram + IMAGE_AREA_PIXELS; ptr++)
        *ptr = COLOR_BLACK;
    update_display();

    SetTimer(g_hwnd, DEFER_RENDER_TIME_ID, 15, (TIMERPROC) Timer0Proc);

    /* Fade steps: blend from black towards target image */
    const int steps = 20;
    for (int step = 1; step <= steps; step++) {
        uint8_t lut[256];
        for (int i = 0; i < 256; i++)
            lut[i] = i * step / steps;

        uint32_t *src = target_buffer;
        uint32_t *dst = g_videoram;
        uint32_t *dst_end = g_videoram + IMAGE_AREA_PIXELS;
        for (; dst < dst_end; src++, dst++) {
            uint32_t pixel = *src;
            *dst = 0xFF000000
                 | ((uint32_t)lut[(pixel >> 16) & 0xFF] << 16)
                 | ((uint32_t)lut[(pixel >> 8) & 0xFF] << 8)
                 | lut[pixel & 0xFF];
        }

        FxDelay(50);
        if (!g_running) break;
    }

    KillTimer(g_hwnd, DEFER_RENDER_TIME_ID);

    /* Ensure final state matches target image */
    if (g_running) {
        memcpy(g_videoram, target_buffer, IMAGE_AREA_PIXELS * sizeof(uint32_t));
        update_display();
    }

    free(target_buffer);
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
            if (posy + y < TEXT_AREA_START) {
                y++;
                x = 0;
            } else {
                break;
            }
        } else if (pctmem[pctpos] == ' ') { /* Transparency */
            if (x + posx < 639) x++;
        } else if (pctmem[pctpos] == '0' || pctmem[pctpos] == '1') {
            if (x + posx < 639 && posy + y < TEXT_AREA_START) {
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

static int GetMasterVolume(void) {
    int pos = 100;
    DWORD ver = GetVersion();
    if (IsWine()) {
        /* Wine: return cached volume since waveOutGetVolume is unreliable */
        return (g_wineVolume >= 0) ? g_wineVolume : 100;
    } else if ((LOBYTE(LOWORD(ver)) >= 4)) {
        /* Windows 95+: use mixer API */
        HMIXER hMixer;
        if (mixerOpen(&hMixer, 0, 0, 0, 0) == MMSYSERR_NOERROR) {
            MIXERLINE ml;
            memset(&ml, 0, sizeof(ml));
            ml.cbStruct = sizeof(ml);
            ml.dwComponentType = MIXERLINE_COMPONENTTYPE_DST_SPEAKERS;
            if (mixerGetLineInfo((HMIXEROBJ)hMixer, &ml, MIXER_GETLINEINFOF_COMPONENTTYPE) == MMSYSERR_NOERROR) {
                MIXERLINECONTROLS mlc;
                MIXERCONTROL mc;
                memset(&mlc, 0, sizeof(mlc));
                memset(&mc, 0, sizeof(mc));
                mlc.cbStruct = sizeof(mlc);
                mlc.dwLineID = ml.dwLineID;
                mlc.dwControlType = MIXERCONTROL_CONTROLTYPE_VOLUME;
                mlc.cControls = 1;
                mlc.cbmxctrl = sizeof(mc);
                mlc.pamxctrl = &mc;
                if (mixerGetLineControls((HMIXEROBJ)hMixer, &mlc, MIXER_GETLINECONTROLSF_ONEBYTYPE) == MMSYSERR_NOERROR) {
                    MIXERCONTROLDETAILS mcd;
                    MIXERCONTROLDETAILS_UNSIGNED mcdu;
                    memset(&mcd, 0, sizeof(mcd));
                    mcd.cbStruct = sizeof(mcd);
                    mcd.dwControlID = mc.dwControlID;
                    mcd.cChannels = 1;
                    mcd.cbDetails = sizeof(mcdu);
                    mcd.paDetails = &mcdu;
                    if (mixerGetControlDetails((HMIXEROBJ)hMixer, &mcd, MIXER_GETCONTROLDETAILSF_VALUE) == MMSYSERR_NOERROR) {
                        pos = (mcdu.dwValue * 100) / mc.Bounds.dwMaximum;
                    }
                }
            }
            mixerClose(hMixer);
        }
    } else {
        /* Win32s: find aux device with "volume" in name */
        UINT numDevs = auxGetNumDevs();
        UINT i;
        for (i = 0; i < numDevs; i++) {
            AUXCAPS caps;
            if (auxGetDevCaps(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
                char nameLower[MAXPNAMELEN];
                strncpy(nameLower, caps.szPname, MAXPNAMELEN - 1);
                nameLower[MAXPNAMELEN - 1] = '\0';
                CharLowerA(nameLower);
                if (strstr(nameLower, g_volumedevice) != NULL) {
                    DWORD vol;
                    if (auxGetVolume(i, &vol) == MMSYSERR_NOERROR) {
                        pos = (LOWORD(vol) * 100 + 0x7FFF) / 0xFFFF;
                    }
                    break;
                }
            }
        }
    }
    return pos;
}

/* Set master volume (0-100) */
static void SetMasterVolume(int pos) {
    DWORD ver = GetVersion();
    if (IsWine()) {
        /* Wine: cache volume and apply to open MCI devices */
        char cmd[64];
        int vol = (pos * 1000) / 100;
        g_wineVolume = pos;
        if (g_mciDeviceID != 0) {
            snprintf(cmd, sizeof(cmd), "setaudio w3vn_music volume to %d", vol);
            mciSendString(cmd, NULL, 0, NULL);
        }
        if (g_videoPlaying) {
            snprintf(cmd, sizeof(cmd), "setaudio video volume to %d", vol);
            mciSendString(cmd, NULL, 0, NULL);
        }
    } else if ((LOBYTE(LOWORD(ver)) >= 4)) {
        /* Windows 95+: use mixer API */
        HMIXER hMixer;
        if (mixerOpen(&hMixer, 0, 0, 0, 0) == MMSYSERR_NOERROR) {
            MIXERLINE ml;
            memset(&ml, 0, sizeof(ml));
            ml.cbStruct = sizeof(ml);
            ml.dwComponentType = MIXERLINE_COMPONENTTYPE_DST_SPEAKERS;
            if (mixerGetLineInfo((HMIXEROBJ)hMixer, &ml, MIXER_GETLINEINFOF_COMPONENTTYPE) == MMSYSERR_NOERROR) {
                MIXERLINECONTROLS mlc;
                MIXERCONTROL mc;
                memset(&mlc, 0, sizeof(mlc));
                memset(&mc, 0, sizeof(mc));
                mlc.cbStruct = sizeof(mlc);
                mlc.dwLineID = ml.dwLineID;
                mlc.dwControlType = MIXERCONTROL_CONTROLTYPE_VOLUME;
                mlc.cControls = 1;
                mlc.cbmxctrl = sizeof(mc);
                mlc.pamxctrl = &mc;
                if (mixerGetLineControls((HMIXEROBJ)hMixer, &mlc, MIXER_GETLINECONTROLSF_ONEBYTYPE) == MMSYSERR_NOERROR) {
                    MIXERCONTROLDETAILS mcd;
                    MIXERCONTROLDETAILS_UNSIGNED mcdu;
                    memset(&mcd, 0, sizeof(mcd));
                    mcd.cbStruct = sizeof(mcd);
                    mcd.dwControlID = mc.dwControlID;
                    mcd.cChannels = 1;
                    mcd.cbDetails = sizeof(mcdu);
                    mcd.paDetails = &mcdu;
                    mcdu.dwValue = (pos * mc.Bounds.dwMaximum) / 100;
                    mixerSetControlDetails((HMIXEROBJ)hMixer, &mcd, MIXER_SETCONTROLDETAILSF_VALUE);
                }
            }
            mixerClose(hMixer);
        }
    } else {
        /* Win32s: find aux device with "volume" in name */
        UINT numDevs = auxGetNumDevs();
        UINT i;
        for (i = 0; i < numDevs; i++) {
            AUXCAPS caps;
            if (auxGetDevCaps(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
                char nameLower[MAXPNAMELEN];
                strncpy(nameLower, caps.szPname, MAXPNAMELEN - 1);
                nameLower[MAXPNAMELEN - 1] = '\0';
                CharLowerA(nameLower);
                if (strstr(nameLower, g_volumedevice) != NULL) {
                    DWORD vol = (pos * 0xFFFF) / 100;
                    vol = vol | (vol << 16);  /* Same for left and right */
                    auxSetVolume(i, vol);
                    break;
                }
            }
        }
    }
}

/* Update or add a line in stvn.ini */
static void UpdateIniLine(char key, const char *value) {
    FILE *fp;
    char lines[32][300];
    int count = 0;
    int found = -1;

    /* Read existing file */
    fp = fopen("stvn.ini", "r");
    if (fp) {
        while (count < 32 && fgets(lines[count], 300, fp)) {
            /* Remove trailing newline */
            size_t len = strlen(lines[count]);
            if (len > 0 && lines[count][len-1] == '\n') lines[count][len-1] = '\0';
            if (lines[count][0] == key) found = count;
            count++;
        }
        fclose(fp);
    }

    /* Update or add line */
    if (found >= 0) {
        snprintf(lines[found], 300, "%c%s", key, value);
    } else {
        if (count < 32) {
            snprintf(lines[count], 300, "%c%s", key, value);
            count++;
        }
    }

    /* Write back */
    fp = fopen("stvn.ini", "w");
    if (fp) {
        for (int i = 0; i < count; i++) {
            fprintf(fp, "%s\n", lines[i]);
        }
        fclose(fp);
    }
}

/* Configuration dialog procedure */
static LRESULT CALLBACK ConfigDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static int volumeGrace = 0;  /* skip timer volume readback after user interaction */
    switch (msg) {
        case WM_CREATE:
            SetTimer(hwnd, 1, 100, NULL);
            return 0;

        case WM_TIMER: {
            if (volumeGrace > 0) {
                volumeGrace--;
            } else {
                HWND hSlider = GetDlgItem(hwnd, IDC_VOLUME_SLIDER);
                int sliderPos = GetScrollPos(hSlider, SB_CTL);
                int masterVol = GetMasterVolume();
                if (sliderPos != masterVol) {
                    SetScrollPos(hSlider, SB_CTL, masterVol, TRUE);
                    InvalidateRect(hSlider, NULL, TRUE);
                }
            }
            /* Deferred window replacement after hq2x enable/disable */
            if (g_repositionWindow) {
                g_repositionWindow = 0;
                RepositionWindow();
            }
            /* Deferred re-center after main window resize (Wine workaround) */
            if (g_recenterDialog) {
                g_recenterDialog = 0;
                RecenterConfigDialog();
            }
            return 0;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == IDCANCEL) {
                SendMessage(hwnd, WM_CLOSE, 0, 0);
                return 0;
            } else if (LOWORD(wParam) == IDC_HQ_CHECKBOX) {
                g_hq2x = !g_hq2x;
                CheckDlgButton(hwnd, IDC_HQ_CHECKBOX, g_hq2x ? BST_CHECKED : BST_UNCHECKED);
                UpdateIniLine('H', g_hq2x ? "1" : "0");
                RestoreWindowSize();

                /* Wine fix, avoid having the window almost out of screen.
                   Deferred because Wine processes the resize asynchronously;
                   the dialog's 100ms timer handles it. */
                if(IsWine()) g_repositionWindow = 1;
                InvalidateRect(g_hwnd, NULL, TRUE);
                return 0;
            }
            break;

        case WM_HSCROLL: {
            HWND hSlider = (HWND)lParam;
            if (hSlider == GetDlgItem(hwnd, IDC_VOLUME_SLIDER)) {
                int pos = GetScrollPos(hSlider, SB_CTL);
                switch (LOWORD(wParam)) {
                    case SB_LINELEFT:  pos = max(0, pos - 1); break;
                    case SB_LINERIGHT: pos = min(100, pos + 1); break;
                    case SB_PAGELEFT:  pos = max(0, pos - 10); break;
                    case SB_PAGERIGHT: pos = min(100, pos + 10); break;
                    case SB_THUMBTRACK:
                    case SB_THUMBPOSITION:
                        pos = HIWORD(wParam);
                        break;
                }
                SetScrollPos(hSlider, SB_CTL, pos, TRUE);
                InvalidateRect(hSlider, NULL, TRUE);
                /* Wine fix: focus rect doesn't track thumb position, cycle focus to force update */
                if (IsWine()) {
                    SetFocus(hwnd);
                    SetFocus(hSlider);
                }
                SetMasterVolume(pos);
                volumeGrace = 10;  /* ignore timer readback for ~1000ms */
            }
            if (hSlider == GetDlgItem(hwnd, IDC_DELAY_SLIDER)) {
                int pos = GetScrollPos(hSlider, SB_CTL);
                switch (LOWORD(wParam)) {
                    case SB_LINELEFT:  pos = max(0, pos - 1); break;
                    case SB_LINERIGHT: pos = min(100, pos + 1); break;
                    case SB_PAGELEFT:  pos = max(0, pos - 10); break;
                    case SB_PAGERIGHT: pos = min(100, pos + 10); break;
                    case SB_THUMBTRACK:
                    case SB_THUMBPOSITION:
                        pos = HIWORD(wParam);
                        break;
                }
                SetScrollPos(hSlider, SB_CTL, pos, TRUE);
                InvalidateRect(hSlider, NULL, TRUE);
                /* Wine fix: focus rect doesn't track thumb position, cycle focus to force update */
                if (IsWine()) {
                    SetFocus(hwnd);
                    SetFocus(hSlider);
                }
                g_textdelay = 100 - pos;
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC:
            SetBkColor((HDC)wParam, RGB(255, 255, 255));
            return (LRESULT)GetStockObject(WHITE_BRUSH);

        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0) == SC_CLOSE) {
                SendMessage(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            if ((wParam & 0xFFF0) == SC_MOVE)
                return 0;
            break;

        case WM_CLOSE:
            EnableWindow(g_hwnd, TRUE);
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY: {
            char volstr[4];
            char delaystr[4];

            KillTimer(hwnd, 1);
            snprintf(volstr, sizeof(volstr), "%03d", GetMasterVolume());
            UpdateIniLine('V', volstr);
            snprintf(delaystr, sizeof(delaystr), "%03d", g_textdelay);
            UpdateIniLine('P', delaystr);

            EnableWindow(g_hwnd, TRUE);
            SetForegroundWindow(g_hwnd);
            g_configDialog = NULL;

            /* Wine fix, sometimes when the dialog closes, the main window does not get the focus back */
            SetWindowPos(g_hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            return 0;
        }

        /* Block window movement (Wine compatibility) */
        case WM_NCHITTEST: {
            LRESULT hit = DefWindowProc(hwnd, msg, wParam, lParam);
            if (hit == HTCAPTION)
                return HTCLIENT;
            return hit;
        }

        /* Wine managed mode: WM can move the dialog bypassing our blocks,
           which desyncs Wine's internal coordinates making it unresponsive.
           Destroy and recreate to get a fresh window at the correct position. */
        case WM_MOVE:
            if (!g_dialogCreating && IsWine()) {
                DestroyWindow(hwnd);
                ShowConfigDialog();
            }
            return 0;

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/* Show the configuration dialog */
static void ShowConfigDialog(void) {
    WNDCLASSEX wcDialog;
    HWND hSlider;
    RECT rect;
    int dialogWidth = 320;
    int dialogHeight = 120;
    int x, y;

    /* Don't open multiple dialogs */
    if (g_configDialog != NULL) {
        SetFocus(g_configDialog);
        return;
    }

    /* Register dialog window class if not already registered */
    memset(&wcDialog, 0, sizeof(wcDialog));
    wcDialog.cbSize = sizeof(WNDCLASSEX);
    wcDialog.lpfnWndProc = ConfigDlgProc;
    wcDialog.hInstance = GetModuleHandle(NULL);
    wcDialog.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wcDialog.lpszClassName = "STVNConfigClass";
    wcDialog.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassEx(&wcDialog);

    /* Adjust for dialog window frame */
    rect.left = 0; rect.top = 0;
    rect.right = dialogWidth; rect.bottom = dialogHeight;
    AdjustWindowRectEx(&rect, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);

    /* Center dialog on parent window's client area */
    POINT topLeft = {0, 0};
    RECT clientRect;
    int dlgW = rect.right - rect.left;
    int dlgH = rect.bottom - rect.top;
    GetClientRect(g_hwnd, &clientRect);
    ClientToScreen(g_hwnd, &topLeft);
    x = topLeft.x + ((clientRect.right - clientRect.left) - dlgW) / 2;
    y = topLeft.y + ((clientRect.bottom - clientRect.top) - dlgH) / 2;

    /* Create the dialog window (system modal) */
    EnableWindow(g_hwnd, FALSE);
    g_dialogCreating = 1;
    g_configDialog = CreateWindowEx(
        WS_EX_DLGMODALFRAME,
        "STVNConfigClass",
        "Configuration",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y,
        rect.right - rect.left, rect.bottom - rect.top,
        g_hwnd, NULL, GetModuleHandle(NULL), NULL);

    g_dialogCreating = 0;

    if (!g_configDialog) {
        EnableWindow(g_hwnd, TRUE);
        return;
    }

    /* Create "Volume" label */
    CreateWindow(
        "STATIC", "Volume",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        10, 15, 80, 20,
        g_configDialog, (HMENU)IDC_VOLUME_LABEL,
        GetModuleHandle(NULL), NULL);

    /* Create volume slider (horizontal scrollbar) */
    hSlider = CreateWindow(
        "SCROLLBAR", NULL,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | SBS_HORZ,
        95, 15, 210, 17,
        g_configDialog, (HMENU)IDC_VOLUME_SLIDER,
        GetModuleHandle(NULL), NULL);
    SetScrollRange(hSlider, SB_CTL, 0, 100, FALSE);
    SetScrollPos(hSlider, SB_CTL, GetMasterVolume(), TRUE);

    /* Create "Text speed" label */
    CreateWindow(
        "STATIC", "Text speed",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        10, 50, 80, 20,
        g_configDialog, (HMENU)IDC_DELAY_LABEL,
        GetModuleHandle(NULL), NULL);

    /* Create delay slider (horizontal scrollbar) */
    hSlider = CreateWindow(
        "SCROLLBAR", NULL,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | SBS_HORZ,
        95, 50, 210, 17,
        g_configDialog, (HMENU)IDC_DELAY_SLIDER,
        GetModuleHandle(NULL), NULL);
    SetScrollRange(hSlider, SB_CTL, 0, 100, FALSE);
    SetScrollPos(hSlider, SB_CTL, 100 - g_textdelay, TRUE);

    /* Create "Enable HQ 2x scaler" label */
    CreateWindow(
        "STATIC", "Enable HQ 2x scaler",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        10, 85, 140, 20,
        g_configDialog, (HMENU)IDC_HQ_LABEL,
        GetModuleHandle(NULL), NULL);

    /* Create checkbox */
    CreateWindow(
        "BUTTON", NULL,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_CHECKBOX,
        155, 85, 20, 20,
        g_configDialog, (HMENU)IDC_HQ_CHECKBOX,
        GetModuleHandle(NULL), NULL);
    CheckDlgButton(g_configDialog, IDC_HQ_CHECKBOX, g_hq2x ? BST_CHECKED : BST_UNCHECKED);

    /* Set focus to the dialog */
    SetFocus(g_configDialog);
}

/* Window procedure */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_KEYDOWN:
            if (wParam == 'C') {
                ShowConfigDialog();
            } else if (!g_effectrunning) {
                switch (wParam) {
                    case VK_SPACE: g_lastkey = 1; break;
                    case 'Q': g_lastkey = 2; break;
                    case 'S': g_lastkey = 3; break;
                    case 'L': g_lastkey = 4; break;
                    case 'B': g_lastkey = 5; break;
                    case 'H': g_lastkey = 6; break;
                    case 'R': g_lastkey = 7; break;
                    case 'E': g_lastkey = 8; break;
                    case VK_ESCAPE: g_lastkey = 9; break;
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
                g_lastkey = 1;
            }
            break;

        case WM_RBUTTONDOWN:
            if (!g_effectrunning) {
                if (g_ignorerclick) {
                    g_ignorerclick = 0;  /* Ignore the click that activated the window */
                } else if (g_windowactive) {
                    g_lastkey = 5;  /* Same as 'B' key for rollback */
                }
            }
            break;

        case WM_MOUSEACTIVATE:
            /* Set ignore flag only for the button that activated the window */
            if (!g_windowactive && LOWORD(lParam) == HTCLIENT) {
                if (HIWORD(lParam) == WM_LBUTTONDOWN) {
                    g_ignoreclick = 1;
                } else if (HIWORD(lParam) == WM_RBUTTONDOWN) {
                    g_ignorerclick = 1;
                }
            }
            return DefWindowProc(hwnd, msg, wParam, lParam);

        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) {
                g_windowactive = 0;
            } else {
                g_windowactive = 1;
                SetFocus(hwnd);  /* Ensure keyboard focus on Win32s */
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
            /* Handle video child window resize */
            if (g_videoPlaying && g_videoWindow) {
                char rcmd[128];
                RECT vwrect;
                CalcVideoWindowRect(&vwrect, g_videoWidth, g_videoHeight);
                SetWindowPos(g_videoWindow, NULL, vwrect.left, vwrect.top,
                             vwrect.right, vwrect.bottom, SWP_NOZORDER);
                /* Update video destination to fill the resized child window */
                snprintf(rcmd, sizeof(rcmd), "put video destination at 0 0 %d %d", vwrect.right, vwrect.bottom);
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
