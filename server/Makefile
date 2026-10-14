# RDPServer: an RDP server for RISC OS, built on xrdp's libxrdp.
#
#   tools/fetch.sh     first: xrdp and mbedTLS at fixed commits, patched
#   make               the RISC OS program, its checks, the app zip
#   make host          the same server for Linux, with a pretend screen
#   make qemu          the server as ARM Linux code, run under qemu-arm-aligntrap
#                      (RISC OS's alignment rules) by tests/e2e.sh QEMU=1
#   make check         unit tests (Linux), the fake RISC OS test, QEMU test
#   make clean
RCD_DIR ?= ../../riscos-crossdev
include $(RCD_DIR)/riscos.mk
.DEFAULT_GOAL := all

VERSION := 0.2
TESTSUFFIX := -test7
B := build
X := $(B)/xrdp
M := $(B)/upstream/mbedtls
MO := $(B)/module

XRDP_SRC := $(wildcard $(X)/libxrdp/*.c) \
  $(addprefix $(X)/common/,parse.c list.c list16.c log.c trans.c string_calls.c \
                            file.c fifo.c pixman-region16.c base64.c guid.c)
OUR_SRC := src/server.c src/srvthread.c src/input.c src/auth.c src/capture.c src/pixels.c src/keymap.c \
           src/keygen.c src/os_calls.c src/ssl_calls.c src/main.c src/coro_ucontext.c
MBED_SRC := $(filter-out %/entropy_poll.c %/net_sockets.c %/timing.c,$(wildcard $(M)/library/*.c))

INC := -include src/config_ac.h -Isrc -I$(X)/common -I$(X)/libxrdp \
       -I$(M)/include -Iconfig -DMBEDTLS_CONFIG_FILE='"rdps_mbedtls_config.h"'
# xrdp's code isn't ours to tidy: no warnings from it.
XRDP_FLAGS := -w

# --- RISC OS ---------------------------------------------------------------
RO := $(B)/riscos
# The desktop front end (the server is the module, below): the windows,
# the module's client, and what -setlogin and the first run's key need.
APP_SRC := src/main.c src/wimp.c src/modclient.c src/auth.c src/keygen.c src/ssl_calls.c \
           src/os_calls.c src/app_stubs.c src/plat_riscos.c
APP_XRDP := $(addprefix $(X)/common/,string_calls.c log.c list.c file.c parse.c)
RO_OBJ := $(patsubst %.c,$(RO)/%.o,$(notdir $(APP_SRC) $(APP_XRDP))) $(RO)/clickup.o $(RO)/modcall.o
RO_MBED := $(patsubst %.c,$(RO)/mbed/%.o,$(notdir $(MBED_SRC)))
RO_CFLAGS := $(RISCOS_CFLAGS) -ffile-prefix-map=$(CURDIR)=rdpserver $(INC)

all: $(B)/RDPServer-$(VERSION)$(TESTSUFFIX).zip

$(RO)/%.o: $(X)/libxrdp/%.c | $(RO)
	$(RO_CC) $(RO_CFLAGS) $(XRDP_FLAGS) -c $< -o $@
$(RO)/%.o: $(X)/common/%.c | $(RO)
	$(RO_CC) $(RO_CFLAGS) $(XRDP_FLAGS) -c $< -o $@
$(RO)/%.o: src/%.c src/*.h | $(RO)
	$(RO_CC) $(RO_CFLAGS) -Wall -Wextra -c $< -o $@
$(RO)/%.o: src/%.s | $(RO)
	$(RO_CC) $(RO_CFLAGS) -c $< -o $@
$(RO)/mbed/%.o: $(M)/library/%.c config/rdps_mbedtls_config.h | $(RO)
	$(RO_CC) $(RO_CFLAGS) -w -c $< -o $@
$(RO)/libmbedcrypto.a: $(RO_MBED)
	rm -f $@ && $(RO_AR) rcs $@ $^
$(RO):
	mkdir -p $(RO)/mbed
$(RO)/rdpserver: $(RO_OBJ) $(RO)/libmbedcrypto.a | riscos-toolchain-present
	$(RO_CC) $(RISCOS_CFLAGS) $(RISCOS_LDFLAGS) -o $@ $(RO_OBJ) $(RO)/libmbedcrypto.a

checks: $(RO)/rdpserver
	$(CHECK_ALL) $(RO)/rdpserver
	$(CHECK_UNALIGNED) -s tools/unaligned-safe.txt $(INC) $(APP_SRC) $(addprefix src/,$(MOD_OUR)) src/mod/module.c src/mod/libc.c src/mod/fmt.c

APP_FILES := !Run,feb !Boot,feb !Help,fff xrdp.ini
ICONS := app/icons/rdpserver.png app/icons/sm-rdpserver.png app/icons/rdpserver-on.png
# The icons: drawn by tools/mkicons.py, made into a sprite file here.
$(B)/Sprites,ff9: $(ICONS)
	$(PNG2SPRITE) $@ '!rdpserver=app/icons/rdpserver.png' \
	  'sm!rdpserver=app/icons/sm-rdpserver.png' 'rdpserver-on=app/icons/rdpserver-on.png'

$(B)/RDPServer-$(VERSION)$(TESTSUFFIX).zip: $(RO)/rdpserver $(MO)/RDPServer,ffa $(B)/Sprites,ff9 app/Licences-head.txt $(addprefix app/!RDPServer/,$(APP_FILES)) | checks
	rm -rf $(B)/app && mkdir -p '$(B)/app/!RDPServer'
	$(RO_STRIP) -o $(RO)/rdpserver.stripped $(RO)/rdpserver
	$(ELF2AIF) -e $(RO)/rdpserver.stripped '$(B)/app/!RDPServer/RDPServer,ff8' >/dev/null
	cp 'app/!RDPServer/!Run,feb' 'app/!RDPServer/!Boot,feb' 'app/!RDPServer/!Help,fff' '$(B)/app/!RDPServer/'
	cp '$(MO)/RDPServer,ffa' '$(B)/app/!RDPServer/RDPSMod,ffa'
	cp 'app/!RDPServer/xrdp.ini' '$(B)/app/!RDPServer/xrdp.ini,fff'
	cp '$(B)/Sprites,ff9' '$(B)/app/!RDPServer/!Sprites,ff9'
	cat app/Licences-head.txt $(B)/upstream/xrdp/COPYING > '$(B)/app/!RDPServer/Licences,fff'
	rm -f $@
	$(ROZIP) $@ -C $(B)/app '!RDPServer'

# --- the module (src/mod): the server, run from the RMA ------------------
# Its own flags, not RISCOS_CFLAGS': no VFP (-mfloat-abi=soft: the VFP
# registers belong to whichever task is paged in), no MOVW/MOVT pairs
# (-mword-relocations: tools/modlink.py relocates absolute words only),
# no PIC; -fstack-clash-protection as RISCOS_CFLAGS has it.
RO_LD := $(RO_BIN)/$(RISCOS_HOST)-ld
MOD_CFLAGS := -O2 -march=armv7-a -mfloat-abi=soft -mno-unaligned-access -fno-pic \
  -mword-relocations -fstack-clash-protection -ffunction-sections -fdata-sections \
  -fno-common -fno-unwind-tables -fno-asynchronous-unwind-tables -DRDPS_MODULE \
  -ffile-prefix-map=$(CURDIR)=rdpserver $(INC)
MOD_OUR := server.c srvthread.c input.c auth.c capture.c pixels.c keymap.c os_calls.c \
           ssl_calls.c plat_riscos.c
MOD_OBJ := $(patsubst %.c,$(MO)/%.o,$(notdir $(XRDP_SRC) $(MOD_OUR))) \
           $(MO)/module.o $(MO)/libc.o $(MO)/fmt.o $(MO)/header.o $(MO)/coro_arm.o $(MO)/clickup.o
MOD_MBED := $(patsubst %.c,$(MO)/mbed/%.o,$(notdir $(MBED_SRC)))
LIBGCC = $(shell $(RO_CC) -print-libgcc-file-name)

module: $(MO)/RDPServer,ffa
$(MO):
	mkdir -p $(MO)/mbed
$(MO)/%.o: $(X)/libxrdp/%.c | $(MO)
	$(RO_CC) $(MOD_CFLAGS) $(XRDP_FLAGS) -c $< -o $@
$(MO)/%.o: $(X)/common/%.c | $(MO)
	$(RO_CC) $(MOD_CFLAGS) $(XRDP_FLAGS) -c $< -o $@
$(MO)/%.o: src/%.c src/*.h | $(MO)
	$(RO_CC) $(MOD_CFLAGS) -Wall -Wextra -c $< -o $@
$(MO)/libc.o: src/mod/libc.c src/mod/*.h | $(MO)
	$(RO_CC) $(MOD_CFLAGS) -fno-builtin -fno-tree-loop-distribute-patterns -Wall -Wextra -c $< -o $@
$(MO)/%.o: src/mod/%.c src/mod/*.h src/*.h | $(MO)
	$(RO_CC) $(MOD_CFLAGS) -fno-tree-loop-distribute-patterns -Wall -Wextra -c $< -o $@
$(MO)/%.o: src/mod/%.s | $(MO)
	$(RO_CC) -march=armv7-a -mfloat-abi=soft -c $< -o $@
$(MO)/%.o: src/%.s | $(MO)
	$(RO_CC) -march=armv7-a -mfloat-abi=soft -c $< -o $@
$(MO)/mbed/%.o: $(M)/library/%.c config/rdps_mbedtls_config.h | $(MO)
	$(RO_CC) $(MOD_CFLAGS) -w -c $< -o $@
$(MO)/libmbed.a: $(MOD_MBED)
	rm -f $@ && $(RO_AR) rcs $@ $^
$(MO)/module.elf: $(MOD_OBJ) $(MO)/libmbed.a src/mod/module.ld | riscos-toolchain-present
	$(RO_LD) -q --gc-sections --no-warn-mismatch -T src/mod/module.ld -o $@ \
	  $(MOD_OBJ) $(MO)/libmbed.a $(LIBGCC)
$(MO)/RDPServer,ffa: $(MO)/module.elf tools/modlink.py
	python3 tools/modlink.py $< $@
	tools/check-module.sh $< $@

# --- Linux, with a pretend screen ------------------------------------------
HO := $(B)/host
HOST_CC ?= gcc
HOST_CFLAGS := -O1 -g $(INC)
HO_OBJ := $(patsubst %.c,$(HO)/%.o,$(notdir $(XRDP_SRC) $(OUR_SRC))) $(HO)/plat_host.o
HO_MBED := $(patsubst %.c,$(HO)/mbed/%.o,$(notdir $(MBED_SRC)))

host: $(HO)/rdpserver
$(HO)/%.o: $(X)/libxrdp/%.c | $(HO)
	$(HOST_CC) $(HOST_CFLAGS) $(XRDP_FLAGS) -c $< -o $@
$(HO)/%.o: $(X)/common/%.c | $(HO)
	$(HOST_CC) $(HOST_CFLAGS) $(XRDP_FLAGS) -c $< -o $@
$(HO)/%.o: src/%.c src/*.h | $(HO)
	$(HOST_CC) $(HOST_CFLAGS) -Wall -Wextra -c $< -o $@
$(HO)/mbed/%.o: $(M)/library/%.c config/rdps_mbedtls_config.h | $(HO)
	$(HOST_CC) $(HOST_CFLAGS) -w -c $< -o $@
$(HO)/libmbedcrypto.a: $(HO_MBED)
	rm -f $@ && ar rcs $@ $^
$(HO):
	mkdir -p $(HO)/mbed
$(HO)/rdpserver: $(HO_OBJ) $(HO)/libmbedcrypto.a
	$(HOST_CC) -o $@ $(HO_OBJ) $(HO)/libmbedcrypto.a

# --- ARM Linux, for qemu-arm-aligntrap -----------------------------------
QO := $(B)/qemu
QEMU_CFLAGS := $(LINUXARM_CFLAGS) $(INC)
QO_OBJ := $(patsubst %.c,$(QO)/%.o,$(notdir $(XRDP_SRC) $(OUR_SRC))) $(QO)/plat_host.o
QO_MBED := $(patsubst %.c,$(QO)/mbed/%.o,$(notdir $(MBED_SRC)))

qemu: $(QO)/rdpserver
$(QO)/%.o: $(X)/libxrdp/%.c | $(QO)
	$(LINUXARM_CC) $(QEMU_CFLAGS) $(XRDP_FLAGS) -c $< -o $@
$(QO)/%.o: $(X)/common/%.c | $(QO)
	$(LINUXARM_CC) $(QEMU_CFLAGS) $(XRDP_FLAGS) -c $< -o $@
$(QO)/%.o: src/%.c src/*.h | $(QO)
	$(LINUXARM_CC) $(QEMU_CFLAGS) -Wall -Wextra -c $< -o $@
$(QO)/mbed/%.o: $(M)/library/%.c config/rdps_mbedtls_config.h | $(QO)
	$(LINUXARM_CC) $(QEMU_CFLAGS) -w -c $< -o $@
$(QO)/libmbedcrypto.a: $(QO_MBED)
	rm -f $@ && arm-linux-gnueabihf-ar rcs $@ $^
$(QO):
	mkdir -p $(QO)/mbed
$(QO)/rdpserver: $(QO_OBJ) $(QO)/libmbedcrypto.a
	$(LINUXARM_CC) $(LINUXARM_CFLAGS) -o $@ $(QO_OBJ) $(QO)/libmbedcrypto.a

# --- tests -------------------------------------------------------------------
T := $(B)/tests
UNIT_SRC := tests/test_units.c src/input.c src/auth.c src/pixels.c src/capture.c src/keymap.c src/keygen.c \
            src/ssl_calls.c src/os_calls.c src/coro_ucontext.c tests/thread_stubs.c

check: checks test-units test-units-qemu test-fake test-fake-wimp test-fake-modclient test-emu test-mod-fmt test-module

$(T):
	mkdir -p $(T)
test-units: $(T)/units
	$(T)/units
$(T)/units: $(UNIT_SRC) src/*.h $(HO)/libmbedcrypto.a | $(T)
	$(HOST_CC) $(HOST_CFLAGS) -Wall -o $@ $(UNIT_SRC) $(HO)/libmbedcrypto.a
test-units-qemu: $(T)/units-arm
	QEMU=$(QEMU_ALIGNTRAP) $(ALIGNTRAP) $(T)/units-arm
$(T)/units-arm: $(UNIT_SRC) src/*.h $(QO)/libmbedcrypto.a | $(T)
	$(LINUXARM_CC) $(QEMU_CFLAGS) -Wall -o $@ $(UNIT_SRC) $(QO)/libmbedcrypto.a
test-fake: $(T)/fake
	$(T)/fake
$(T)/fake: tests/test_fake.c src/plat_riscos.c src/platform.h $(FAKE_SRC) | $(T)
	$(FAKE_CC) $(FAKE_CFLAGS) -o $@ tests/test_fake.c src/plat_riscos.c $(FAKE_SRC)

test-fake-wimp: $(T)/fake-wimp
	$(T)/fake-wimp
$(T)/fake-wimp: tests/test_fake_wimp.c src/wimp.c src/wimp.h src/server.h $(FAKE_SRC) | $(T)
	$(FAKE_CC) $(FAKE_CFLAGS) -Isrc -o $@ tests/test_fake_wimp.c src/wimp.c $(FAKE_SRC)

test-fake-modclient: $(T)/fake-modclient
	$(T)/fake-modclient
$(T)/fake-modclient: tests/test_fake_modclient.c src/modclient.c src/mod/modapi.h src/server.h $(FAKE_SRC) | $(T)
	$(FAKE_CC) $(FAKE_CFLAGS) -Isrc -o $@ tests/test_fake_modclient.c src/modclient.c $(FAKE_SRC)

# The module's printf/scanf against the C library's (32-bit, as RISC OS).
test-mod-fmt: $(T)/mod-fmt
	$(T)/mod-fmt
$(T)/mod-fmt: tests/test_mod_fmt.c src/mod/fmt.c src/mod/fmt.h | $(T)
	$(FAKE_CC) -m32 -O1 -Wall -Wno-format -Isrc -o $@ tests/test_mod_fmt.c src/mod/fmt.c

# The module itself in the Unicorn emulator (tests/modemu.py), with a key
# made by the host build.
$(T)/keys/rsakeys.ini: $(HO)/rdpserver | $(T)
	mkdir -p $(T)/keys
	RDPS_CHOICES=$(T)/keys RDPS_RESOURCES='app/!RDPServer' $(HO)/rdpserver -p 34999 -t 1 >/dev/null 2>&1
test-module: $(MO)/RDPServer,ffa $(T)/keys/rsakeys.ini
	python3 tests/test_module.py $(MO)/RDPServer,ffa $(T)/keys

# The click-release routines in the Unicorn emulator (pip install unicorn).
test-emu: $(RO)/clickup.o
	python3 tests/test_clickup.py $(RO)/clickup.o

clean:
	rm -rf $(RO) $(HO) $(QO) $(MO) $(B)/app $(B)/*.zip $(B)/tests

.PHONY: module all host qemu check checks test-units test-units-qemu test-fake test-fake-wimp test-fake-modclient test-emu test-mod-fmt test-module clean
