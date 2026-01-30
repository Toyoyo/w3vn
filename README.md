# W3VN
A very simple visual novel engine for Win32s and ulterior win32 implementations.

![stvn](/assets/stvn.png)

## Building
To build this, you need the following:
* GNU make
* OpenWatcom 2.0 beta
* UPX for compression

## Prerequistes
* Enhanced mode Windows 3.1 with win32s 1.30c or anything more recent (tested on WfW3.11, wine and windows 10)

## Running
* Provide a script file following the documented syntax
* Edit STVN.INI to meet your needs: script file and Window title
* Run W3VN.EXE

## Key bindings
* 'Space' to advance
* 'q' Quit
* 's' Save state
* 'l' Load state
* 'b' Go back 1 text block
* 'h' Show help screen
* 'r' Reset window size

## STVN.INI settings
As a sample, use the following:
```
SSTDEMO.VNS
TSTVN Engine - Win32s
```
'S' line is the script file

'T' line is the window Title

Defaults: ``STVN.VNS`` & ``STVN Engine - Win32s``

## Supported formats / limitations:
* For pictures: PI3 monochrome, optionally gzipped, or PNG (via libpng)
  for PI1, the palette isn't used, only the first 25600 bytes are read (640x360 image, leaving 40 pixels for text, 4 lines)

* For audio: Anything MCI supports, like MIDI or RAW/ADPCM WAV, for the most compatible formats.

Savestates: 4 supported, adding more would be trivial.
10 VN choices binary "registers", adding more wouldn't be very hard.
Scripts can be 999999 lines long.

All resource files must be placed in the 'data' subdirectory.

## Syntax:
Each line starts with an operand, like:
``IATARI.PI3``

Supported operands:
* 'W' : Wait for input

  This is where most interaction will happen.

  Like saving/loading, waiting for 'space', quitting, etc...

* 'I' : Change background image

  Syntax: ``I[file]`` like ``IFILE.PI3``

  Expected format is PI3, can (should) be gzipped, or PNG, and is loaded entirely before being copied to video memory.

  For PI3, only the first 320 lines are loaded and drawn, the rest of the screen being used for the text area.

  A few bytes can thus be saved, for example bv removing the 400-lines padding code in pbmtopi3 source.

  As the picture is loaded in RAM before being copied in vram, this operand will use 32000 bytes of RAM during operation, freed after.

  This will obviously erase all sprites and clear the internal sprite list.

* 'S' : Sayer change

  Syntax: ``Sname`` like `SToyoyo`

  Obviously, this will reset character lines to 0.

  This is used as the save pointer and will also reset the text box.

* 'T' : Text line

  Syntax: ``Ttext`` like ``THere's a text line``

  Line wrapping is disabled, so make sure a line isn't more than 79 characters!

* 'P' : Change music

  Syntax: ``P[file]`` like ``PFILE.MID``

  Any music/sound file supported by MCI, notably MIDI and WAV files.

  ADPCM WAV provides some compression while being compatible with any windows version starting from Windows 3.1.

* 'J' : Jump to label

  Syntax: ``J[label]`` like `JLBL1```

  A label 4 bytes sequence at the beginning of a line

* 'C' : Offer a choice and store it in a register

  Key '1' entered: store 0

  Key '2' entered: store 1

  Key '3' entered: store 2

  Key '4' entered: store 3

  Syntax: ``C[register number][Max choices]`` like ``C04`` meaning "Get a choice between 1 and 4 and assign it to register 0"

  Also offer the same interactions as a 'W' line.

* 'R' : Redraw the background picture.

  More precisely, copy back the buffer of last loaded one into video memory.

  This will obviously erase all sprites and clear the internal sprite list.

* 'B' : Jump to label if register is set.

  Syntax: ``B[register number][Choice][label]`` like ``B02LBL1`` meaning "if register 0 is at 2, jump to LBL1"

* 'D' : Wait the specified number of milliseconds (max 99999).

  Syntax: ``D[msec]`` like ``D2000``

* 'A' : Load and draw a sprite in video memory.

  Syntax: ``A[XXX][YYY][file]`` like ``A000000TOYO.SPR``

  The expected format can be:

  - PNG

  - Or the original monochrome STVN format derived from XPM which can (should) be gzipped and is plotted as it's read.

  For the monochrome XPM-based format, a converter script is provided and screen boundary are checked during drawing, but you can draw on the text area if you want.

  More precicely: X coordinates after 640 are ignored and Y coordinates after 400 ends the drawing routine.

  This can use quite some memory: 1 byte per pixel, allocated in 1024 bytes blocks, freed after operation.

  And this is still drawn slower than a full screen image.

  They're tracked as much as I could during load & back routines, so what *should* happen is:

  * If you load a save, both background & sprites ('A' lines) encoutered between the lasts 'I' or 'R' lines are drawn.
  * If you go back and neither background & sprites changes: nothing is drawn
  * If you go back and there's a background change, this will also trigger a sprite draw pass.
  * If you go back to the 'S' line just before a 'A' Line, it will trigger a draw pass too. This is by design: When the 'S' line happens, the sprite doesn't exist yet.

* 'E' : Manually erase the text box, reset character lines to 0

* 'X' : Trigger a visual effect.

  Syntax: ``X[XX]`` like ``X03``

  01: Vertically fade to black from top to bottom, 2 lines by 2 lines

  02: Vertically fade to white from top to bottom, 2 lines by 2 lines

  03: 01 then 02

  04: 02 then 01

  05: Vertically fade to black from bottom to top, 2 lines by 2 lines

  06: Vertically fade to white from bottom to top, 2 lines by 2 lines

  07: 05 then 06

  08: 06 then 05

  09: Vertically fade to black from outer to inner, 2 lines by 2 lines

  10: Vertically fade to white from outer to inner, 2 lines by 2 lines

  11: 09 then 10

  12: 10 then 09

  13: Vertically fade to black from inner to outer, 2 lines by 2 lines

  14: Vertically fade to white from inner to outer, 2 lines by 2 lines

  15: 13 then 14

  16: 14 then 13
