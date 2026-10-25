CC      = clang
RC      = $(CC:clang=windres)
VERSION = 0.0.0
CFLAGS  = --target=x86_64-w64-mingw32 -std=c17 -O2 -Wall -Wextra -Wno-unused-parameter \
          -ffunction-sections -fdata-sections -Ivendor/minhook/include
LDFLAGS = -shared -s -Wl,--gc-sections
LDLIBS  = -lole32 -loleaut32 -luuid -lshell32 -luser32 -ladvapi32

VER     = $(subst ., ,$(VERSION))
RCFLAGS = -DVER=$(VERSION) -DVER_MAJOR=$(word 1,$(VER)) -DVER_MINOR=$(word 2,$(VER)) -DVER_PATCH=$(word 3,$(VER))

SRC = src/ebfs.c src/hooks.c src/feature_plan.c src/size_cache.c src/symbols.c src/everything.c \
      $(addprefix vendor/minhook/src/,buffer.c hook.c trampoline.c hde/hde64.c)
DLL = build/ExplorerBetterFileSizes.dll

build:
	mkdir -p build
	$(RC) $(RCFLAGS) -o build/version.o src/version.rc
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(DLL) $(SRC) build/version.o src/exports.def $(LDLIBS)

dist: build
	mkdir -p dist
	cp $(DLL) install.cmd uninstall.cmd README.md LICENSE dist/
	cp vendor/minhook/LICENSE.txt dist/LICENSE.minhook.txt
	cp config.ini dist/

clean:
	rm -rf build dist

.PHONY: build dist clean
