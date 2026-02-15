/*
 *      STVN Engine - Win32s Port
 *      (c) 2022, 2023, 2026 Toyoyo
 */

#ifndef FUNC_H
#define FUNC_H

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

#pragma comment(lib, "winmm.lib")

/* Screen dimensions */
#define SCREEN_WIDTH  640
#define SCREEN_HEIGHT 400
#define TEXT_AREA_START 320

/* Colors in BGRA format */
#define COLOR_WHITE 0xFFFFFFFF
#define COLOR_BLACK 0xFF000000

/* Music timer ID */
#define MUSIC_TIMER_ID 1
#define DEFER_RENDER_TIME_ID 2

#define IMAGE_AREA_PIXELS (SCREEN_WIDTH * TEXT_AREA_START)
#define TEXT_AREA_PIXELS (SCREEN_WIDTH * 80)

#endif /* FUNC_H */
