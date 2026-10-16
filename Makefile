#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
TARGET		:=	$(notdir $(CURDIR))
APP_TITLE	:=	NetherSX2
APP_AUTHOR	:=	naga
APP_VERSION	:=	1.4.0
BUILD		?=	build-$(RENDERER)
SOURCES		:=	source source/hooks source/switch
DATA		:=	data
INCLUDES	:=	source source/switch \
			launcher/dependencies/build/_deps/libsmb2-src/include \
			launcher/dependencies/build/_deps/libusbhsfs-src/include

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
LTOFLAGS := -flto=auto -fuse-linker-plugin

# __SWITCH__ for libnx; NETHERSX2 gates the port-specific shim branches.
DEFINES	:=	-D__SWITCH__ -DNETHERSX2 -DCURL_STATICLIB \
			-DNETHERSX2_VERSION='"$(APP_VERSION)"'

ifneq ($(strip $(NETHERSX2_VK_DIAGNOSTIC)),)
DEFINES += -DNETHERSX2_VK_DIAGNOSTIC
endif

RENDERER ?= GL
MESA_SDK_ROOT ?=
# pkg-config for the nxvk portlib package. Defaults to the devkitA64 cross
# wrapper -- the same one launcher/Makefile already uses for SDL2 -- but CI can
# override it when the package is only reachable another way.
NXVK_PKG_CONFIG ?= $(PREFIX)pkg-config
ifneq ($(strip $(MESA_SDK_ROOT)),)
ifneq ($(RENDERER),DK)
DEFINES += -DUSE_UNIFIED_MESA
endif
endif
ifeq ($(RENDERER),DK)
TARGET := $(notdir $(CURDIR))_dk
DEFINES += -DUSE_DEKO3D -DGS_RENDERER=12
SOURCES += source/deko3d
DEKO3D_DEPS_ROOT ?= $(TOPDIR)/out/deko3d-deps
INCLUDES += source/deko3d third_party/lsfg-deko3d/include $(DEKO3D_DEPS_ROOT)/lsfg/include
else
ifeq ($(RENDERER),VK)
DEFINES	+=	-DUSE_VULKAN -DGS_RENDERER=14 -DVK_USE_PLATFORM_VI_NN
SOURCES	+=	source/lsfg \
			third_party/lsfg-vk/lsfg-vk-common/src/helpers \
			third_party/lsfg-vk/lsfg-vk-common/src/vulkan \
			third_party/lsfg-vk/lsfg-vk-backend/src \
			third_party/lsfg-vk/lsfg-vk-backend/src/extraction \
			third_party/lsfg-vk/lsfg-vk-backend/src/helpers \
			third_party/lsfg-vk/lsfg-vk-backend/src/shaderchains
INCLUDES	+=	source/lsfg \
			third_party/lsfg-vk/lsfg-vk-common/include \
			third_party/lsfg-vk/lsfg-vk-backend/include \
			third_party/lsfg-vk/lsfg-vk-backend/src
# Detect the nxvk portlib package here, not down by LIBS: CFLAGS below is
# assigned with := and expands $(DEFINES) immediately, so the shim macro has to
# be in DEFINES before that line.
NXVK_PKG	:=	$(shell $(NXVK_PKG_CONFIG) --exists nxvk 2>/dev/null && echo 1)
ifeq ($(NXVK_PKG),1)
# libnvk.a defines only vk_icdGetInstanceProcAddr -- see source/hooks/vk.c.
DEFINES		+=	-DNETHERSX2_NVK_ICD_ONLY
endif
else
DEFINES	+=	-DGS_RENDERER=12
endif
endif

# -fdata-sections pairs with the --gc-sections that nxvk.pc puts on the VK link
# line; it is harmless for DK/GL, which do not enable gc-sections themselves.
CFLAGS	:=	-g -Wall -O3 -ffunction-sections -fdata-sections -fno-omit-frame-pointer $(LTOFLAGS) \
			$(ARCH) $(DEFINES)
CFLAGS	+=	$(INCLUDE)
CXXFLAGS	:= $(CFLAGS) -std=gnu++20

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) $(LTOFLAGS) -Wl,-Map,$(notdir $*.map)

STORAGE_LIBS := $(TOPDIR)/launcher/dependencies/build/_deps/libsmb2-build/lib/libsmb2.a \
				$(TOPDIR)/launcher/dependencies/build/_deps/libusbhsfs-build/liblibusbhsfs.a \
				$(PORTLIBS)/lib/libntfs-3g.a

# nx: libnx (audren for the AAudio shim, HID, applet, fs). m: libm. No SDL2/
# OpenSL ES -- audio is the in-tree AAudio->audren shim (source/aaudio.c).
ifeq ($(RENDERER),DK)
LIBDIRS := $(PORTLIBS) $(LIBNX)
DEKO3D_LIBS := $(DEKO3D_DEPS_ROOT)/lsfg/liblsfg.a \
              $(DEKO3D_DEPS_ROOT)/lsfg/liblsfg_spirv_cross.a \
              $(DEKO3D_DEPS_ROOT)/uam-generated/uam_isolated.o
LIBS := -Wl,--start-group $(DEKO3D_LIBS) -Wl,--end-group \
        $(STORAGE_LIBS) -lcurl -lpng -lz -ldeko3d -lnx -lstdc++ -lm
else
ifeq ($(RENDERER),VK)
ifneq ($(strip $(MESA_SDK_ROOT)),)
# Unified Mesa SDK: the Vulkan driver and all private Mesa dependencies are
# packaged in one archive. External dependencies stay in devkitPro portlibs.
LIBDIRS	:= $(MESA_SDK_ROOT) $(PORTLIBS) $(LIBNX)
LIBS	:= -Wl,--start-group -lvulkan -lEGL -lGLESv2 -lglapi \
		-lmesa_util_c11 -lblake3 -lmesa_util -lmesa_util_simd -lxmlconfig \
		-Wl,--end-group $(STORAGE_LIBS) -lcurl -lelf -lexpat -lz -lzstd \
		-lnx -lstdc++ -lm
else
# Mesa NVK, two supported layouts (detected into $(NXVK_PKG) above):
#
# portlib package, preferred -- the artifact built by berlint-hub/nxvk's
# switch-vulkan.yml: libnvk.a + libnvk_support.a + pkgconfig/nxvk.pc, extracted
# into $(PORTLIBS). nxvk.pc carries the entire link line, including
# --whole-archive, -u,vk_icdGetInstanceProcAddr and --gc-sections, so none of it
# is spelled out here. vulkan/ and vk_video/ headers arrive through
# -I$(PORTLIBS)/include, which LIBDIRS below generates.
#
# vendored tree, fallback -- 23 static archives (vulkan/lib) linked in one
# --start-group (circular NVK<->runtime<->nir<->compiler deps). -l:libX.a links
# by exact file name (avoids the -lvulkan GROUP-script + the double-prefixed
# liblibnil...a). That libvulkan_lite_* does export the plain
# vkGetInstanceProcAddr, which is why the ICD-only shim stays off for it.
# -lz/-lzstd resolve crc32/ZSTD_*; the DRM/nouveau_ws path is dead-stripped so no
# -ldrm_nouveau. -lstdc++/libgcc unwinder are needed by NAK's bundled Rust.
ifeq ($(NXVK_PKG),1)
LIBDIRS	:= $(PORTLIBS) $(LIBNX)
LIBS	:= $(STORAGE_LIBS) `$(NXVK_PKG_CONFIG) --libs nxvk` \
		-lcurl -lz -lzstd -lnx -lstdc++ -lm
else
LIBDIRS	:= $(TOPDIR)/vulkan $(PORTLIBS) $(LIBNX)
LIBS	:= -Wl,--start-group \
		-l:libnvk.a -l:libvulkan_lite_runtime.a -l:libvulkan_runtime.a \
		-l:libvulkan_lite_instance.a -l:libvulkan_instance.a \
		-l:libvulkan_util.a -l:libvulkan_wsi.a \
		-l:libnak.a -l:libnak_rs.a -l:libvtn.a -l:libxmlconfig.a \
		-l:libnil.a -l:liblibnil_format_table.a -l:libnouveau_mme.a \
		-l:libnouveau_ws.a -l:libnvidia_headers_c.a \
		-l:libnir.a -l:libcompiler.a -l:libcompiler_c_helpers.a \
		-l:libmesa_util.a -l:libmesa_util_simd.a -l:libblake3.a -l:libmesa_util_c11.a \
		-Wl,--end-group $(STORAGE_LIBS) -lcurl -lz -lzstd -lnx -lstdc++ -lm
endif
endif
else
ifneq ($(strip $(MESA_SDK_ROOT)),)
# The unified EGL archive contains both native NVC0 and Zink. Keep Vulkan in
# the same rescan group so the GL host can select either driver at runtime.
LIBDIRS	:= $(MESA_SDK_ROOT) $(PORTLIBS) $(LIBNX)
LIBS	:= -Wl,--start-group -lvulkan -lEGL -lGLESv2 -lglapi \
		-lmesa_util_c11 -lblake3 -lmesa_util -lmesa_util_simd -lxmlconfig \
		-Wl,--end-group $(STORAGE_LIBS) -lcurl -lelf -lexpat -lz -lzstd \
		-lnx -lstdc++ -lm
else
# EGL/GLESv2/glapi/drm_nouveau: switch-mesa/nouveau GL.
LIBDIRS	:= $(PORTLIBS) $(LIBNX)
LIBS	:= $(STORAGE_LIBS) -lcurl -lz -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lnx -lm
endif
endif
endif

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# link with g++ so mesa's C++ EGL/GLES pulls in libstdc++
export LD	:=	$(CXX)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(if $(filter /%,$(dir)),$(dir),$(CURDIR)/$(dir))) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.jpg)
	ifneq (,$(findstring $(TARGET).jpg,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).jpg
	else
		ifneq (,$(findstring icon.jpg,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.jpg
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
	export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(APP_TITLEID),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

# libnx provides a weak 1 KiB __nx_exception_stack, while crash.c supplies the
# strong 512 KiB stack used by the re-entrant fastmem/JIT fault handler. Keep
# that defining object out of LTO so the linker sees the final strong size
# before the LTO plugin materializes symbols (and does not retain/report the
# weak libnx size).
crash.o: CFLAGS += -fno-lto

#---------------------------------------------------------------------------------
all	:	$(OUTPUT).nro

ifeq ($(strip $(NO_NACP)),)
$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
else
$(OUTPUT).nro	:	$(OUTPUT).elf
endif

# Relink when the prebuilt storage archives change, not only on source edits.
$(OUTPUT).elf	:	$(OFILES) $(STORAGE_LIBS) $(DEKO3D_LIBS)

$(OFILES_SRC)	: $(HFILES_BIN)
$(OFILES_SRC)	: $(TOPDIR)/Makefile

%.bin.o	%_bin.h :	%.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
