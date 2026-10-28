# PrismPack - build in WSL:
#   wsl -e bash -lc 'cd /mnt/c/projects/PrismPack && PATH=/opt/amiga/bin:$PATH make'
# GPL-3.0-only, see LICENSE.

CC      = m68k-amigaos-gcc
CFLAGS  = -m68000 -O2 -noixemul -Wall -Wno-pointer-sign -fomit-frame-pointer
OUT     = out

all: $(OUT)/PrismUnpack

$(OUT)/PrismUnpack: src/unpack.c src/inflate.c src/inflate.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -s -o $@ src/unpack.c src/inflate.c

# the decoder against zlib, natively (gcc + python3 in WSL)
test:
	@mkdir -p build
	gcc -O2 -Wall -Wextra -shared -fPIC -o build/libinflate.so src/inflate.c
	python3 test/inflate_host.py $(TESTDIR)

clean:
	rm -rf $(OUT) build

.PHONY: all test clean
