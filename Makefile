SOURCES_DIR=./src
BUILD_DIR=./build
DIST_DIR=./dist
RSC_DIR=./rsc
ZLIB=./deps/zlib-1.3.1
PNG=./deps/libpng-1.6.54

CC=wcl386
CFLAGS=-bt=nt -l=nt_win -za99 -I$(ZLIB) -I$(PNG)
WRC=wrc

# VASM PARAMETERS
ASM=vasmm68k_mot
ASMFLAGS=-Felf -quiet -x -m68000 -spaces -showopt

all: prepare zlib libpng dist

prepare:
	mkdir -p $(BUILD_DIR)

zlib:
	cd $(ZLIB) ; wmake -f watcom/watcom_f.mak

libpng:
	cp deps/Makefile.libpng $(PNG)/Makefile
	cp deps/pnglibconf.h $(PNG)/
	cd $(PNG) ; wmake

main:
	$(WRC) -q -zm -bt=nt -r -fo=w3vn.res w3vn.rc
	$(CC) $(CFLAGS) -fe=build/w3vn.exe src/w3vn.c $(ZLIB)/zlib_f.lib $(PNG)/libpng.lib w3vn.res
	$(CC) $(CFLAGS) -dSCALED_RENDERING -fe=build/w3vn1280.exe src/w3vn.c $(ZLIB)/zlib_f.lib $(PNG)/libpng.lib w3vn.res

dist: main
	mkdir -p $(DIST_DIR)
	cp $(BUILD_DIR)/w3vn.exe $(DIST_DIR)
	cp $(BUILD_DIR)/w3vn1280.exe $(DIST_DIR)
	cp -R $(RSC_DIR)/* $(DIST_DIR)
	upx --strip-relocs=0 $(DIST_DIR)/w3vn.exe
	upx --strip-relocs=0 $(DIST_DIR)/w3vn1280.exe

clean:
	rm -rf $(BUILD_DIR)
	rm -rf $(DIST_DIR)
	rm -f w3vn.o w3vn.res
	if [ -f "$(ZLIB)/zlib_f.lib" ]; then\
		cd $(ZLIB) ; wmake -f watcom/watcom_f.mak clean;\
	fi
	if [ -f "$(PNG)/libpng.lib" ]; then\
		cd $(PNG) ; wmake clean;\
	fi
	rm -f $(PNG)/Makefile
	rm -f $(PNG)/pnglibconf.h
