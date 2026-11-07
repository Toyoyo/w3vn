# Makefile for zlib
# OpenWatcom flat model
# Last updated: 28-Dec-2005

# To use, do "wmake -f watcom_f.mak"

C_SOURCE =  adler32.c  compress.c crc32.c   deflate.c    &
	    gzclose.c  gzlib.c    gzread.c  gzwrite.c    &
            infback.c  inffast.c  inflate.c inftrees.c   &
            trees.c    uncompr.c  zutil.c

OBJS =      adler32.o  compress.o crc32.o   deflate.o    &
	    gzclose.o  gzlib.o    gzread.o  gzwrite.o    &
            infback.o  inffast.o  inflate.o inftrees.o   &
            trees.o    uncompr.o  zutil.o

CC       = wcc386
LINKER   = wcl386
CFLAGS   = -zq -mf -3r -fp3 -s -bt=nt -oilrtfm -fr=nul -wx -ox
ZLIB_LIB = zlib_f.lib

.c.o:
        $(CC) $(CFLAGS) $[@

all: $(ZLIB_LIB)

$(ZLIB_LIB): $(OBJS)
	wlib -b -c $(ZLIB_LIB) -+adler32.o  -+compress.o -+crc32.o
	wlib -b -c $(ZLIB_LIB) -+gzclose.o  -+gzlib.o    -+gzread.o   -+gzwrite.o
        wlib -b -c $(ZLIB_LIB) -+deflate.o  -+infback.o
        wlib -b -c $(ZLIB_LIB) -+inffast.o  -+inflate.o  -+inftrees.o
        wlib -b -c $(ZLIB_LIB) -+trees.o    -+uncompr.o  -+zutil.o

clean: .SYMBOLIC
          rm *.o
          rm $(ZLIB_LIB)
          @echo Cleaning done
