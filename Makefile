.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Defina DEVKITPRO (ex: export DEVKITPRO=/opt/devkitpro)")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

APP_TITLE   := Netflix Player
APP_AUTHOR  := Voce
APP_VERSION := 0.3.0

TARGET   := netflix-player
BUILD    := build
SOURCES  := source
INCLUDES := source
LIBDIRS  := $(PORTLIBS) $(LIBNX)

PKG := $(DEVKITPRO)/portlibs/switch/bin/aarch64-none-elf-pkg-config
PKGS := mpv sdl2 SDL2_image SDL2_ttf libcurl

ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS   := -g -Wall -O2 -ffunction-sections $(ARCH) -D__SWITCH__ $(shell $(PKG) --cflags $(PKGS))
CXXFLAGS := $(CFLAGS) -std=gnu++17
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS     := $(shell $(PKG) --libs --static $(PKGS)) -lnx -lm

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT := $(CURDIR)/$(TARGET)

# Embute icone + NACP dentro do .nro. Sem isso o Homebrew Menu e as ferramentas
# de atalho (forwarder) nao reconhecem o app.
ICON := $(CURDIR)/icon.jpg
ifneq ($(wildcard $(ICON)),)
export NROFLAGS += --icon=$(ICON)
endif
export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
# romfs/ (intro.mp4 + logo.png) vai dentro do .nro
ifneq ($(wildcard $(CURDIR)/romfs),)
export NROFLAGS += --romfsdir=$(CURDIR)/romfs
endif
export TOPDIR := $(CURDIR)
export VPATH  := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
export LD := $(CXX)
export OFILES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) $(foreach dir,$(LIBDIRS),-I$(dir)/include)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

else

.PHONY: all
DEPENDS := $(OFILES:.o=.d)
all: $(OUTPUT).nro
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)
-include $(DEPENDS)

endif
