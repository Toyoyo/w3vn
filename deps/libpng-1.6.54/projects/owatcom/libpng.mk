.before
 # Locations of zlib and (if required) awk (change as required:)
 set zlib=..\..\..\zlib
 set awk=
 #
 @if not exist pngconfig.dfa $(MAKE) $(__MAKEOPTS__) -f pngconfig.mak defaul&
ts
 @if exist config.inf type config.inf
 @echo Checking for the libpng configuration file pnglibconf.h
 $(MAKE) $(__MAKEOPTS__) -f pngconfig.mak
.after
 @type pngconfig.inf
project : Z:\home\fosco\src\claude\w3vn\deps\libpng-1.6.54\projects\owatcom\&
libpng.lib Z:\home\fosco\src\claude\w3vn\deps\libpng-1.6.54\projects\owatcom&
\pngtest.exe Z:\home\fosco\src\claude\w3vn\deps\libpng-1.6.54\projects\owatc&
om\pngvalid.exe Z:\home\fosco\src\claude\w3vn\deps\libpng-1.6.54\projects\ow&
atcom\pngstest.exe .SYMBOLIC

!include Z:\home\fosco\src\claude\w3vn\deps\libpng-1.6.54\projects\owatcom\l&
ibpng.mk1
!include Z:\home\fosco\src\claude\w3vn\deps\libpng-1.6.54\projects\owatcom\p&
ngtest.mk1
!include Z:\home\fosco\src\claude\w3vn\deps\libpng-1.6.54\projects\owatcom\p&
ngvalid.mk1
!include Z:\home\fosco\src\claude\w3vn\deps\libpng-1.6.54\projects\owatcom\p&
ngstest.mk1
