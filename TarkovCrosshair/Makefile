CC      ?= x86_64-w64-mingw32-gcc
WINDRES ?= x86_64-w64-mingw32-windres
CFLAGS  ?= -std=c11 -O2 -Wall
LIBS     = -luser32 -lgdi32 -lshell32 -lcomdlg32 -lmsimg32

build/TarkovCrosshair.exe: src/main.c src/core.h build/app.res.o
	$(CC) $(CFLAGS) -municode -mwindows src/main.c build/app.res.o -o $@ $(LIBS) -s

build/app.res.o: res/app.rc res/app.ico res/app.manifest | build
	cd res && $(WINDRES) app.rc -O coff -o ../build/app.res.o

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: clean
