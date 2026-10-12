# m7mddz File Manager - OpenOrbis PS4 build (modeled on the OpenOrbis hello_world sample)
# Needs the OpenOrbis release (bin/linux, include, lib, link.x) and env OO_PS4_TOOLCHAIN.

TITLE       := m7mddz
VERSION     := 1.00
TITLE_ID    := MDDZ00001
CONTENT_ID  := IV0000-MDDZ00001_00-M7MDDZFILEMGR000

LIBS        := -lc -lkernel
TOOLCHAIN   := $(OO_PS4_TOOLCHAIN)
INTDIR      := build
CDIR        := linux

CFILES      := $(wildcard src/*.c)
OBJS        := $(patsubst src/%.c, $(INTDIR)/%.o, $(CFILES))

CFLAGS      := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -c -O2 -DM7_PS4 -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include
LDFLAGS     := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr -L$(TOOLCHAIN)/lib $(LIBS) $(TOOLCHAIN)/lib/crt1.o

CC          := clang
LD          := ld.lld

_unused     := $(shell mkdir -p $(INTDIR))

all: $(CONTENT_ID).pkg

$(CONTENT_ID).pkg: pkg.gp4
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core pkg_build $< .

pkg.gp4: eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png
	$(TOOLCHAIN)/bin/$(CDIR)/create-gp4 -out $@ --content-id=$(CONTENT_ID) --files "$^"

sce_sys/about/right.sprx:
	mkdir -p sce_sys/about
	cp "$$(find $(TOOLCHAIN)/bin/data -name right.sprx | head -n1)" $@

sce_sys/param.sfo: Makefile
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_new $@
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

eboot.bin: src/index_html.h $(OBJS)
	$(LD) $(INTDIR)/*.o -o $(INTDIR)/$(TITLE).elf $(LDFLAGS)
	$(TOOLCHAIN)/bin/$(CDIR)/create-fself -in=$(INTDIR)/$(TITLE).elf -out=$(INTDIR)/$(TITLE).oelf --eboot "eboot.bin" --paid 0x3800000000000011

src/index_html.h: www/index.html tools/gen_html.py
	python3 tools/gen_html.py

$(INTDIR)/%.o: src/%.c src/index_html.h
	$(CC) $(CFLAGS) -o $@ $<

# Desktop test, no PS4 needed:  make host && ./build/m7host
host: src/index_html.h
	gcc -O1 -Wall -o $(INTDIR)/m7host src/main.c

clean:
	rm -rf $(INTDIR) $(CONTENT_ID).pkg pkg.gp4 eboot.bin sce_sys/param.sfo sce_sys/about

.PHONY: all host clean
