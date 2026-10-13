# RDPSProbe: what an RDP server on RISC OS would have to work with.
#
#   make            out/rdpsprobe (ELF), the checks, out/RDPSProbe-$(VERSION).zip
#   make check      the checks, the fake RISC OS test and the QEMU test
#   make clean
#
# Needs riscos-crossdev (RCD_DIR, a tagged release).
RCD_DIR ?= ../../riscos-crossdev
include $(RCD_DIR)/riscos.mk
.DEFAULT_GOAL := all

VERSION := 0.2
SRC := src/main.c src/probe.c src/ro.c src/measure.c
HDR := src/probe.h src/ro.h src/measure.h
OUT := out
APP := app/!RDPSProbe

all: $(OUT)/RDPSProbe-$(VERSION).zip
check: checks test-fake test-qemu

$(OUT)/rdpsprobe: $(SRC) $(HDR) $(RO_CC) $(RISCOS_TOOLCHAIN)/$(RISCOS_HOST)/lib/libunixlib.a | riscos-toolchain-present
	@mkdir -p $(OUT)
	$(RO_CC) $(RISCOS_CFLAGS) -Wall -Wextra -ffile-prefix-map=$(CURDIR)=rdpsprobe $(RISCOS_LDFLAGS) -o $@ $(SRC)

checks: $(OUT)/rdpsprobe
	$(CHECK_ALL) $(OUT)/rdpsprobe
	$(CHECK_UNALIGNED) -Isrc $(SRC)

$(OUT)/RDPSProbe-$(VERSION).zip: $(OUT)/rdpsprobe $(APP)/!Run,feb $(APP)/!Help,fff $(APP)/!Boot,feb | checks
	rm -rf $(OUT)/!RDPSProbe && mkdir -p $(OUT)/!RDPSProbe
	$(RO_STRIP) -o $(OUT)/rdpsprobe.stripped $(OUT)/rdpsprobe
	$(ELF2AIF) -e $(OUT)/rdpsprobe.stripped $(OUT)/!RDPSProbe/RDPSProbe,ff8 >/dev/null
	cp $(APP)/!Run,feb $(APP)/!Help,fff $(APP)/!Boot,feb $(OUT)/!RDPSProbe/
	rm -f $@
	$(ROZIP) $@ -C $(OUT) !RDPSProbe

test-fake: $(OUT)/test_fake
	$(OUT)/test_fake
$(OUT)/test_fake: tests/test_fake.c src/ro.c src/probe.c src/measure.c $(HDR) $(FAKE_SRC)
	@mkdir -p $(OUT)
	$(FAKE_CC) $(FAKE_CFLAGS) -o $@ tests/test_fake.c src/ro.c src/probe.c src/measure.c $(FAKE_SRC)

test-qemu: $(OUT)/test_qemu
	QEMU=$(QEMU_ALIGNTRAP) $(ALIGNTRAP) $(OUT)/test_qemu
$(OUT)/test_qemu: tests/test_qemu.c src/measure.c src/measure.h
	@mkdir -p $(OUT)
	$(LINUXARM_CC) $(LINUXARM_CFLAGS) -Wall -o $@ tests/test_qemu.c src/measure.c

clean:
	rm -rf $(OUT)

.PHONY: all check checks test-fake test-qemu clean
