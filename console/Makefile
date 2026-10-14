#---------------------------------------------------------------------------------
# 3DS VR - console app for the Nintendo 3DS family
#   make          -> .3dsx, .cia and .3ds in dist/
#   make 3dsx     -> Homebrew Launcher build only
#
# Requires devkitARM + libctru + citro2d/citro3d (devkitPro 3ds-dev),
# 3ds-libjpeg-turbo, and makerom / bannertool in PATH (or MAKEROM=... BANNERTOOL=...).
#---------------------------------------------------------------------------------
DEVKITPRO  ?= /opt/devkitpro
DEVKITARM  ?= $(DEVKITPRO)/devkitARM
CTRULIB    ?= $(DEVKITPRO)/libctru
PORTLIBS   ?= $(DEVKITPRO)/portlibs/3ds
TOOLS      ?= $(DEVKITPRO)/tools/bin

MAKEROM    ?= makerom
BANNERTOOL ?= bannertool
SMDHTOOL   ?= $(TOOLS)/smdhtool
3DSXTOOL   ?= $(TOOLS)/3dsxtool
PICASSO    ?= $(TOOLS)/picasso
BIN2S      ?= $(TOOLS)/bin2s

TARGET      := 3DS-VR
APP_TITLE   := 3DS VR
APP_DESC    := Use your 3DS as a SteamVR headset
APP_AUTHOR  := Homebrew

VERSION_MAJOR := 1
VERSION_MINOR := 0
VERSION_MICRO := 0
VERSION       := $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_MICRO)
TITLE_VER     := $(shell echo $$(( ($(VERSION_MAJOR) << 10) | ($(VERSION_MINOR) << 4) | $(VERSION_MICRO) )))

BUILD    := build
DIST     := dist
SOURCES  := source
SHADERS  := shaders
META     := meta
PROTOCOL := ../protocol

PREFIX  := $(DEVKITARM)/bin/arm-none-eabi-
CC      := $(PREFIX)gcc
AS      := $(PREFIX)as

ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS  := -g -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11 -mword-relocations \
           -ffunction-sections -fdata-sections $(ARCH) -D__3DS__ \
           -DAPP_VERSION=\"$(VERSION)\" -DAPP_VERSION_MAJOR=$(VERSION_MAJOR) \
           -DAPP_VERSION_MINOR=$(VERSION_MINOR) -DAPP_VERSION_MICRO=$(VERSION_MICRO) \
           -I$(CTRULIB)/include -I$(PORTLIBS)/include -I$(PROTOCOL) -I$(BUILD)
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -Wl,--gc-sections -Wl,-Map,$(BUILD)/$(TARGET).map \
           -L$(CTRULIB)/lib -L$(PORTLIBS)/lib
LIBS    := -lcitro2d -lcitro3d -ljpeg -lctru -lm

SRCS    := $(wildcard $(SOURCES)/*.c)
OBJS    := $(patsubst $(SOURCES)/%.c,$(BUILD)/%.o,$(SRCS))
DEPS    := $(OBJS:.o=.d)
PICAS   := $(wildcard $(SHADERS)/*.v.pica)
SHOBJS  := $(patsubst $(SHADERS)/%.v.pica,$(BUILD)/%.shbin.o,$(PICAS))
SHHDRS  := $(patsubst $(SHADERS)/%.v.pica,$(BUILD)/%_shbin.h,$(PICAS))

ELF  := $(BUILD)/$(TARGET).elf

.PHONY: all 3dsx cia cci clean

all: 3dsx cia cci
3dsx: $(DIST)/$(TARGET).3dsx
cia:  $(DIST)/$(TARGET).cia
cci:  $(DIST)/$(TARGET).3ds

$(BUILD) $(DIST):
	@mkdir -p $@

# shader -> .shbin -> object + header exposing <name>_shbin / <name>_shbin_size
$(BUILD)/%.shbin.o $(BUILD)/%_shbin.h: $(SHADERS)/%.v.pica | $(BUILD)
	@echo "PICA $<"
	@$(PICASSO) -o $(BUILD)/$*.shbin $<
	@cd $(BUILD) && $(BIN2S) -a 4 -H $*_shbin.h $*.shbin | $(AS) -o $*.shbin.o

$(BUILD)/%.o: $(SOURCES)/%.c $(SHHDRS) | $(BUILD)
	@echo "CC  $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(ELF): $(OBJS) $(SHOBJS)
	@echo "LD  $@"
	@$(CC) $(LDFLAGS) $(OBJS) $(SHOBJS) $(LIBS) -o $@

$(BUILD)/$(TARGET).smdh: $(META)/icon.png | $(BUILD)
	@$(SMDHTOOL) --create "$(APP_TITLE)" "$(APP_DESC)" "$(APP_AUTHOR)" $< $@

$(DIST)/$(TARGET).3dsx: $(ELF) $(BUILD)/$(TARGET).smdh | $(DIST)
	@echo "3DSX $@"
	@$(3DSXTOOL) $(ELF) $@ --smdh=$(BUILD)/$(TARGET).smdh

$(BUILD)/icon.icn: $(META)/icon.png | $(BUILD)
	@$(BANNERTOOL) makesmdh -s "$(APP_TITLE)" -l "$(APP_DESC)" -p "$(APP_AUTHOR)" -i $< -o $@ >/dev/null

$(BUILD)/banner.bnr: $(META)/banner.png $(META)/banner.wav | $(BUILD)
	@$(BANNERTOOL) makebanner -i $(META)/banner.png -a $(META)/banner.wav -o $@ >/dev/null

$(DIST)/$(TARGET).cia: $(ELF) $(BUILD)/icon.icn $(BUILD)/banner.bnr $(META)/app.rsf | $(DIST)
	@echo "CIA $@"
	@$(MAKEROM) -f cia -o $@ -elf $(ELF) -rsf $(META)/app.rsf -icon $(BUILD)/icon.icn \
		-banner $(BUILD)/banner.bnr -exefslogo -target t -ver $(TITLE_VER)

$(DIST)/$(TARGET).3ds: $(ELF) $(BUILD)/icon.icn $(BUILD)/banner.bnr $(META)/app.rsf | $(DIST)
	@echo "3DS $@"
	@$(MAKEROM) -f cci -o $@ -elf $(ELF) -rsf $(META)/app.rsf -icon $(BUILD)/icon.icn \
		-banner $(BUILD)/banner.bnr -exefslogo -target d -ver $(TITLE_VER)

clean:
	@rm -rf $(BUILD) $(DIST)

-include $(DEPS)
