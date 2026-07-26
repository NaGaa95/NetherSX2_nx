# Forwarder stub -> ../romfs/fwd/{hbl.nso,hbl.npdm,default.nacp}. devkitPro only.
DEVKITPRO ?= /opt/devkitpro

CC     := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-gcc
TOOLS  := $(DEVKITPRO)/tools/bin
OUT    := ../romfs/fwd

# cfg dir is hardcoded in main.c; don't pass it via -D (MSYS2 mangles leading-slash args).
VERSION := 1.2.1
LTOFLAGS := -flto=auto -fuse-linker-plugin

CFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs \
          -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft \
          -fPIE -O3 -DNDEBUG -Wall -Wextra -ffunction-sections -fdata-sections -fno-ident $(LTOFLAGS) \
          -ffile-prefix-map=$(CURDIR)=. -fmacro-prefix-map=$(CURDIR)=. \
          -I$(DEVKITPRO)/libnx/include \
          -D__SWITCH__ -DVERSION='"$(VERSION)"'
LDFLAGS := -L$(DEVKITPRO)/libnx/lib -lnx $(LTOFLAGS) -Wl,--gc-sections -Wl,--build-id=none -Wl,--wrap,exit

all: $(OUT)/hbl.nso $(OUT)/hbl.npdm $(OUT)/default.nacp

hbl.elf: main.c trampoline.s
	$(CC) main.c trampoline.s -o $@ $(CFLAGS) $(LDFLAGS)

$(OUT)/hbl.nso: hbl.elf | $(OUT)
	$(TOOLS)/elf2nso $< $@

$(OUT)/hbl.npdm: hbl.json | $(OUT)
	$(TOOLS)/npdmtool $< $@

$(OUT)/default.nacp: | $(OUT)
	$(TOOLS)/nacptool --create NetherSX2 NetherSX2 $(VERSION) $@

$(OUT):
	@mkdir -p $(OUT)

clean:
	@rm -f hbl.elf
	@rm -rf $(OUT)

.PHONY: all clean
