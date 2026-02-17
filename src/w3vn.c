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

/* Global window and rendering state */
static HWND g_hwnd = NULL;
static HICON g_hIcon = NULL;
static uint32_t *g_videoram = NULL;
static char g_windowTitle[128] = "STVN Engine - Win32s";
static uint32_t *g_background = NULL;
static uint32_t *g_textarea = NULL;
static volatile int g_running = 1;
static volatile int g_lastkey = 0;
static volatile int g_mouseclick = 0;
static volatile int g_windowactive = 1;
static volatile int g_ignoreclick = 0;
static volatile int g_ignorerclick = 0;
static volatile int g_effectrunning = 0;
static volatile int g_hq2x = 0;
static char g_volumedevice[128] = "volume";
static int g_origvolume = 100;

/* Character rendering state */
static int g_cursorX = 0;
static int g_cursorY = 0;
static int g_textdelay = 0; /* Delay between characters in ms (0 = instant) */
static int g_textskip = 0;  /* 0=no delay, 1=delay active, -1=user skipped */

/* Audio state */
static UINT g_mciDeviceID = 0;
static char g_currentMusic[260] = {0};

/* Video state */
static int g_videoPlaying = 0;
static int g_videoWidth = 0;
static int g_videoHeight = 0;
static HWND g_videoWindow = NULL;

#include "func.c"

/* Macros */
#define RestoreScreen() memcpy(g_videoram, g_background, IMAGE_AREA_PIXELS * sizeof(uint32_t))
#define SaveScreen() memcpy(g_background, g_videoram, IMAGE_AREA_PIXELS * sizeof(uint32_t))

/* Used to check a valid choice in Loading/Saving dialogs */
#define NoValidSaveChoice(n) (((n) != 2 && (n) != 9) && ((n) < 10 || (n) > 19))

#define SaveMacro() {\
    next = read_keyboard_status();\
    while (NoValidSaveChoice(next) && g_running) {\
        if (next == 7) RestoreWindowSize();\
        next = read_keyboard_status();\
        Sleep(5);\
    }\
    if (next != 2 && next != 9) {\
        HandleSaveFilename(next);\
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
    }\
}

#define DeleteMacro() {\
    next = read_keyboard_status();\
    while (NoValidSaveChoice(next) && g_running) {\
        if (next == 7) RestoreWindowSize();\
        next = read_keyboard_status();\
        Sleep(5);\
    }\
    if (next != 2 && next != 9) {\
        HandleSaveFilename(next);\
        if(file_exists(savefile) == 0) {\
            if (remove(savefile) != 0) {\
                DispEraseError();\
            }\
        }\
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

#define QuitMacro() {\
    next = read_keyboard_status();\
    while ((next != 9 && next != 10 && next != 11) && g_running) {\
        if (next == 7) RestoreWindowSize();\
        next = read_keyboard_status();\
        Sleep(5);\
    }\
    if (next == 10) goto endprog;\
}

#define EscMacro() {\
    next = read_keyboard_status();\
    while ((next != 9 && next != 10 && next != 11) && g_running) {\
        if (next == 7) RestoreWindowSize();\
        next = read_keyboard_status();\
        Sleep(5);\
    }\
    if (next == 10) {\
        rewind(script);\
        lineNumber = 0;\
        savepointer = 0;\
        willplaying = 0;\
        spritecount = 0;\
        memset(musicfile, 0, sizeof(musicfile));\
        memset(oldmusicfile, 0, sizeof(oldmusicfile));\
        memset(picture, 0, sizeof(picture));\
        memset(oldpicture, 0, sizeof(oldpicture));\
        reset_cursprites();\
        reset_prevsprites();\
        StopMusic();\
        isplaying = 0;\
        savehistory_idx = 0;\
        memset(savehistory, 0, sizeof(savehistory));\
        memset(choicedata, 0, 11);\
        memset(sayername, 0, sizeof(sayername));\
        skipnexthistory = 0;\
        loadsave = 0;\
        backfromvideo = 0;\
        g_textskip = 0;\
        charlines = 0;\
        memset(g_background, 0xFF, IMAGE_AREA_PIXELS * sizeof(uint32_t));\
        clear_screen();\
    }\
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
    char sayername[260] = {0};
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
    int backfromvideo = 0;

    char spritefile[260] = {0};
    int posx = 0;
    int posy = 0;
    char linex[4] = {0};
    char liney[4] = {0};

    reset_cursprites();
    reset_prevsprites();

    int spritecount = 0;
    char scriptfile[260] = "data\\stvn.vns";

    int restorevolume=0;

    choicedata = (char *)malloc(11);
    if (choicedata == NULL) return;
    memset(choicedata, 0, 11);

    clear_screen();

    /* Parse config file */
    if (file_exists("stvn.ini") == 0) {
        config = fopen("stvn.ini", "r");
        line = get_line(config);

        int vol=200;
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
                if (*line == 'H') {
                    if (strlen(line) > 1 && line[1] == '1') g_hq2x = 1;
                }
                if (*line == 'R') {
                    if (strlen(line) > 1 && line[1] == '1') restorevolume=1;
                }
                if (*line == 'V') {
                    if (strlen(line) >= 4) {
                        vol = atoi(line + 1);
                    }
                }
                if (*line == 'P') {
                    if (strlen(line) >= 4) {
                        g_textdelay = atoi(line + 1);
                        if (g_textdelay > 100) g_textdelay = 100;
                    }
                }
                if (*line == 'D') {
                    strncpy(g_volumedevice, line + 1, sizeof(g_volumedevice) - 1);
                    g_volumedevice[sizeof(g_volumedevice) - 1] = '\0';
                }
            }
            line = get_line(config);
        }
        fclose(config);

        /*Restore volume if it was set in the config file */
        if(restorevolume) g_origvolume = GetMasterVolume();
        if (vol >= 0 && vol <= 100) SetMasterVolume(vol);
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

    RestoreWindowSize();

    /* Wine fix, avoid having the window almost out of screen */
    if(IsWine() && g_hq2x == 1) CenterWindow();

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
            /* Reset text delay when leaving a text block */
            if (*line != 'T' && *line != 'N') g_textskip = 0;

            /* 'W': Wait for input */
            if (*line == 'W') {
                update_display();
                g_mouseclick = 0;  /* Clear any pending click */
                next = read_keyboard_status();
                while (next != 1 && !g_mouseclick && g_running) {
                    if (next == 2) {
                        SaveScreen();
                        DispQuit();
                        QuitMacro();
                        next = 0;
                        RestoreScreen();
                        update_display();
                    }

                    /* Save */
                    if (next == 3) {
                        SaveScreen();
                        DispLoadSave(1);
                        SaveMacro();
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

                    if (next == 9) {
                        SaveScreen();
                        DispEsc();
                        EscMacro();
                        next = 0;
                        if (lineNumber == 0) break;
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
                        RedrawBorder();
                        print_string(" Rolling back...");
                        update_display();
                        goto seektoline;
                    }

                    /* Help */
                    if (next == 6) {
                        SaveScreen();
                        DispHelp();
                        while (next != 2 && next != 9 && g_running) {
                            if (next == 7) RestoreWindowSize();
                            next = read_keyboard_status();
                            Sleep(5);
                        }
                        next = 0;
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
                        if (next != 2 && next != 9) {
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
                                uint32_t bgcolor = COLOR_WHITE;
                                memset(picture, 0, sizeof(picture));

                                /* On load (not rollback): stop music - will restart if 'P' is encountered */
                                if (loadsave == 1) {
                                    if (isplaying) {
                                        StopMusic();
                                        isplaying = 0;
                                    }
                                    memset(musicfile, 0, sizeof(musicfile));
                                    memset(oldmusicfile, 0, sizeof(oldmusicfile));
                                }

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

                                    if (*line == 'X') {
                                        reset_cursprites();
                                        spritecount = 0;
                                        if (strlen(line) >= 3) {
                                            char effect[3] = {0};
                                            memcpy(effect, line + 1, 2);
                                            int effectnum = atoi(effect);
                                            memset(picture, 0, sizeof(picture));
                                            memset(oldpicture, 0, sizeof(oldpicture));
                                            /* Track background color */
                                            if(effectnum == 1) bgcolor = COLOR_BLACK;
                                            if(effectnum == 2) bgcolor = COLOR_WHITE;
                                            if(effectnum == 3) bgcolor = COLOR_WHITE;
                                            if(effectnum == 4) bgcolor = COLOR_BLACK;
                                            if(effectnum == 5) bgcolor = COLOR_BLACK;
                                            if(effectnum == 6) bgcolor = COLOR_WHITE;
                                            if(effectnum == 7) bgcolor = COLOR_WHITE;
                                            if(effectnum == 8) bgcolor = COLOR_BLACK;
                                            if(effectnum == 9) bgcolor = COLOR_BLACK;
                                            if(effectnum == 10) bgcolor = COLOR_WHITE;
                                            if(effectnum == 11) bgcolor = COLOR_WHITE;
                                            if(effectnum == 12) bgcolor = COLOR_BLACK;
                                            if(effectnum == 13) bgcolor = COLOR_BLACK;
                                            if(effectnum == 14) bgcolor = COLOR_WHITE;
                                            if(effectnum == 15) bgcolor = COLOR_WHITE;
                                            if(effectnum == 16) bgcolor = COLOR_BLACK;
                                            if(effectnum == 17) bgcolor = COLOR_BLACK;
                                            if(effectnum == 18) bgcolor = COLOR_WHITE;
                                            if(effectnum == 19) bgcolor = COLOR_WHITE;
                                            if(effectnum == 20) bgcolor = COLOR_BLACK;
                                            if(effectnum == 21) bgcolor = COLOR_BLACK;
                                            if(effectnum == 22) bgcolor = COLOR_WHITE;
                                            if(effectnum == 23) bgcolor = COLOR_WHITE;
                                            if(effectnum == 24) bgcolor = COLOR_BLACK;
                                            if(effectnum == 25) bgcolor = COLOR_BLACK;
                                            if(effectnum == 26) bgcolor = COLOR_WHITE;
                                            if(effectnum == 27) bgcolor = COLOR_WHITE;
                                            if(effectnum == 28) bgcolor = COLOR_BLACK;
                                            if(effectnum == 29) bgcolor = COLOR_BLACK;
                                            if(effectnum == 30) bgcolor = COLOR_WHITE;
                                            if(effectnum == 31) bgcolor = COLOR_WHITE;
                                            if(effectnum == 32) bgcolor = COLOR_BLACK;
                                            if(effectnum == 33) bgcolor = COLOR_BLACK;
                                            if(effectnum == 34) bgcolor = COLOR_WHITE;
                                            if(effectnum == 35) bgcolor = COLOR_WHITE;
                                            if(effectnum == 36) bgcolor = COLOR_BLACK;
                                            if(effectnum == 37) bgcolor = COLOR_BLACK;
                                            if(effectnum == 38) bgcolor = COLOR_WHITE;
                                            if(effectnum == 39) bgcolor = COLOR_WHITE;
                                            if(effectnum == 40) bgcolor = COLOR_BLACK;
                                            if(effectnum == 98) bgcolor = COLOR_BLACK;
                                            /* X99 loads a new background image, track it like 'I' */
                                            if(effectnum == 99 && strlen(line) >= 4) {
                                                int filelen = (int)strlen(line) - 3;
                                                if (filelen > 250) filelen = 250;
                                                snprintf(picture, sizeof(picture), "data\\%.*s", filelen, line + 3);
                                            }
                                        }
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
                                        if (line[1] == 'S') {
                                            /* PS: Stop music */
                                            memset(musicfile, 0, sizeof(musicfile));
                                            memset(oldmusicfile, 0, sizeof(oldmusicfile));
                                            willplaying = 0;
                                        } else {
                                            int filelen = (int)strlen(line) - 1;
                                            if (filelen > 0) {
                                                if (filelen > 250) filelen = 250;
                                                memset(musicfile, 0, sizeof(musicfile));
                                                snprintf(musicfile, sizeof(musicfile), "data\\%.*s", filelen, line + 1);
                                                willplaying = 1;
                                            }
                                        }
                                    }

                                    if (*line == 'S') {
                                        savepointer = lineNumber;
                                        strncpy(sayername, line + 1, sizeof(sayername) - 1);
                                        sayername[sizeof(sayername) - 1] = '\0';
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
                                if (picture[0] == '\0') {
                                    for (int _i = 0; _i < IMAGE_AREA_PIXELS; _i++) g_background[_i] = bgcolor;
                                    memset(oldpicture, 0, sizeof(oldpicture));
                                    RestoreScreen();
                                } else if (loadsave == 0) {
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
                                if (compare_sprites() != 0 || loadsave == 1 || backfromvideo == 1) {
                                    for (int sc = 0; sc < spritecount; sc++) {
                                        memset(spritefile, 0, sizeof(spritefile));
                                        snprintf(spritefile, sizeof(spritefile), "data\\%s", currentsprites[sc].file);
                                        DisplaySprite(spritefile, currentsprites[sc].x, currentsprites[sc].y);
                                    }
                                }

                                /* Clear text area and display sayer name from replay
                                   This mostly superfluous, but can help in case of corrupted saves */
                                memcpy(g_videoram + IMAGE_AREA_PIXELS, g_textarea, TEXT_AREA_PIXELS * sizeof(uint32_t));
                                RedrawBorder();
                                if (sayername[0]) {
                                    locate(0, 322);
                                    print_string(sayername);
                                }

                                /* Play or stop music if needed */
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
                                } else {
                                    if(isplaying == 1) {
                                        StopMusic();
                                        isplaying = 0;
                                    }
                                    memset(musicfile, 0, sizeof(musicfile));
                                    memset(oldmusicfile, 0, sizeof(oldmusicfile));
                                }

                                backfromvideo = 0;
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
                update_display();

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
                if (g_textskip == 0) g_textskip = 1;
                locate(0, 337 + charlines * 15);
                print_string(" ");
                print_string(line + 1);
                charlines++;
            }

            /* 'N': Immediate text line (no delay) */
            if (*line == 'N') {
                int prev_textskip = g_textskip;
                g_textskip = 0;
                locate(0, 337 + charlines * 15);
                print_string(" ");
                print_string(line + 1);
                g_textskip = prev_textskip;
                charlines++;
            }

            /* 'P': Play music, 'PS': Stop music */
            if (*line == 'P') {
                if (line[1] == 'S') {
                    /* PS: Stop music */
                    if (isplaying) {
                        StopMusic();
                        isplaying = 0;
                    }
                    memset(musicfile, 0, sizeof(musicfile));
                    memset(oldmusicfile, 0, sizeof(oldmusicfile));
                } else {
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
                            g_ignoreclick = 0;
                            g_ignorerclick = 0;
                        }
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

                    /* Also invalidate oldpicture to force a background redraw in case of rollback */
                    memset(oldpicture, 0, sizeof(oldpicture));

                    /* Wine fix: reposition window before video playback if partially off-screen to avoid hanging
                       0180:err:quartz:image_presenter_PresentImage Failed to blit */
                    if (IsWine()) RepositionWindow();

                    PlayVideo(videofile);
                    /* Wait for video to finish or space to skip */
                    while (IsVideoPlaying() && g_running && !stopvideo) {
                        /* Process all messages, check for space key */
                        if (PeekMessage(&vmsg, NULL, 0, 0, PM_REMOVE)) {
                            if (vmsg.message == WM_QUIT) {
                                g_running = 0;
                            } else if (vmsg.message == WM_KEYDOWN && !g_configDialog && vmsg.wParam == VK_SPACE) {
                                stopvideo = 1;
                            } else if (vmsg.message == WM_KEYDOWN && !g_configDialog && vmsg.wParam == 'R') {
                                RestoreWindowSize();
                            } else if (vmsg.message == WM_KEYDOWN && !g_configDialog && vmsg.wParam == 'B') {
                                /* Only stop video for rollback if rollback is possible */
                                if (savehistory_idx >= 2) {
                                    stopvideo = 1;
                                    rollbackvideo = 1;
                                }
                            } else if (vmsg.message == WM_KEYDOWN && !g_configDialog && vmsg.wParam == 'Q') {
                                char vcmd[128];
                                RECT wrect, vwrect;
                                mciSendString("pause video", NULL, 0, NULL);
                                /* Hide video child window */
                                if (g_videoWindow) ShowWindow(g_videoWindow, SW_HIDE);
                                g_effectrunning = 0;
                                SaveScreen();
                                DispQuit();
                                QuitMacro();
                                RestoreScreen();
                                update_display();
                                g_effectrunning = 1;
                                /* Restore video child window position/size and show it */
                                if (g_videoWindow) {
                                    CalcVideoWindowRect(&vwrect, g_videoWidth, g_videoHeight);
                                    SetWindowPos(g_videoWindow, NULL, vwrect.left, vwrect.top,
                                                 vwrect.right, vwrect.bottom, SWP_NOZORDER);
                                    snprintf(vcmd, sizeof(vcmd), "put video destination at 0 0 %d %d", vwrect.right, vwrect.bottom);
                                    mciSendString(vcmd, NULL, 0, NULL);
                                    ShowWindow(g_videoWindow, SW_SHOW);
                                }
                                mciSendString("resume video", NULL, 0, NULL);
                            } else if (!ConfigDialogMessage(&vmsg)) {
                                TranslateMessage(&vmsg);
                                DispatchMessage(&vmsg);
                            }
                        } else {
                            /* No messages - yield briefly */
                            Sleep(1);
                        }
                    }

                    StopVideo();
                    RestoreScreen();
                    g_effectrunning = 0;
                    g_lastkey = 0;
                    g_ignoreclick = 0;
                    g_ignorerclick = 0;

                    /* Handle rollback if 'B' was pressed during video */
                    if (rollbackvideo && savehistory_idx >= 2) {
                        save_linenb = savehistory[savehistory_idx - 2];
                        savehistory[savehistory_idx - 1] = 0;
                        savehistory_idx--;
                        skipnexthistory = 1;
                        backfromvideo = 1;  /* Force sprite redraw in seektoline */

                        memcpy(g_videoram + IMAGE_AREA_PIXELS, g_textarea, TEXT_AREA_PIXELS * sizeof(uint32_t));
                        locate(0, 337);
                        RedrawBorder();
                        print_string(" Rolling back...");
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
                        if (next == 2) {
                            SaveScreen();
                            DispQuit();
                            QuitMacro();
                            next = 0;
                            RestoreScreen();
                            update_display();
                        }

                        if (next == 7) RestoreWindowSize();

                        if (next == 3) {
                            SaveScreen();
                            DispLoadSave(1);
                            SaveMacro();
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

                        if (next == 9) {
                            SaveScreen();
                            DispEsc();
                            EscMacro();
                            if (lineNumber == 0) break;
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

                            if (ldnext != 2 && ldnext != 9) {
                                HandleSaveFilename(ldnext);

                                RestoreScreen();
                                update_display();
                                if (file_exists(savefile) == 0) {
                                    next = ldnext;
                                    loadsave = 1;
                                    goto lblloadsave;
                                }
                            } else {
                                RestoreScreen();
                                update_display();
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
                            while (next != 2 && next != 9 && g_running) {
                                if (next == 7) RestoreWindowSize();
                                next = read_keyboard_status();
                                Sleep(5);
                            }
                            next = 0;
                            RestoreScreen();
                            update_display();
                        }

                        Sleep(5);
                    }
                    if (lineNumber > 0)
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
                    if (effectnum == 98) FxFadeOut();
                    if (effectnum == 99) {
                        if (strlen(line) >= 4) {
                            int filelen = (int)strlen(line) - 3;
                            if (filelen > 250) filelen = 250;
                            memset(picture, 0, sizeof(picture));
                            snprintf(picture, sizeof(picture), "data\\%.*s", filelen, line + 3);
                            memcpy(oldpicture, picture, sizeof(oldpicture));
                            FxFadeIn(picture);
                        }
                    }

                    FlushMessages();
                    reset_cursprites();
                    spritecount = 0;
                    g_effectrunning = 0;
                    g_lastkey = 0;  /* Clear any key pressed during effect */
                    g_ignoreclick = 0;
                    g_ignorerclick = 0;
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

    /* Save volume, in case it was changed externally */
    char volstr[4];
    snprintf(volstr, sizeof(volstr), "%03d", GetMasterVolume());
    UpdateIniLine('V', volstr);
    if(restorevolume) SetMasterVolume(g_origvolume);
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

    /* Register video child window class with black background */
    WNDCLASSEX vwc = {0};
    vwc.cbSize = sizeof(WNDCLASSEX);
    vwc.lpfnWndProc = DefWindowProc;
    vwc.hInstance = hInstance;
    vwc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    vwc.lpszClassName = "STVNVideoClass";
    RegisterClassEx(&vwc);

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

    /* Request best available timer resolution */
    TIMECAPS tc;
    UINT timerPeriod = 100;
    if (timeGetDevCaps(&tc, sizeof(tc)) == TIMERR_NOERROR) {
        timerPeriod = tc.wPeriodMin;
    }
    timeBeginPeriod(timerPeriod);

    /* Run the engine */
    run();

    /* Cleanup - destroy window first to prevent WM_PAINT accessing freed memory */
    if (g_hwnd && IsWindow(g_hwnd)) {
        DestroyWindow(g_hwnd);
        g_hwnd = NULL;
    }

    free(g_videoram);
    g_videoram = NULL;
    free(g_background);
    g_background = NULL;
    free(g_textarea);
    g_textarea = NULL;

    timeEndPeriod(timerPeriod);

    if (g_hIcon) DestroyIcon(g_hIcon);
    UnregisterClass("STVNClass", hInstance);
    UnregisterClass("STVNVideoClass", hInstance);

    return 0;
}
