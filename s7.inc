O ?= .
TMP ?= .
BL33 ?=
QEMU_LD_PREFIX ?= /usr/x86_64-linux-gnu/

UNAME_M := $(shell uname -m)
ifneq ($(UNAME_M),x86_64)
	QEMU = qemu-x86_64 -L $(QEMU_LD_PREFIX)
endif

ACPU_IMAGETOOL ?= ./acpu-imagetool
AML_ENCRYPT ?= ./aml_encrypt_s7

BL30_PAYLOAD_SIZE := 65536
BL33_PAYLOAD_SIZE := 1572864
DVINIT_PARAMS_SIZE := 8192
ALIGN := 4096

.PHONY: clean distclean all
.NOTPARALLEL: ${TMP}/dvinit-params.bin ${TMP}/bb1st.sto.bin ${TMP}/bb1st.usb.bin \
	${TMP}/bl30-payload.bin ${TMP}/bl33-payload.bin ${TMP}/device-fip.bin \
	${O}/u-boot.bin

all: ${O}/u-boot.bin

clean:
	rm -f ${TMP}/dvinit-params.bin
	rm -f ${TMP}/bb1st.sto.bin ${TMP}/bb1st.usb.bin
	rm -f ${TMP}/bl30-payload.bin ${TMP}/bl33-payload.bin ${TMP}/bl33.lz4 ${TMP}/bl33.bin
	rm -f ${TMP}/device-fip.bin ${TMP}/ddr-fip.bin
	rm -f ${TMP}/aml-payload.cfg*

distclean: clean
	rm -f ${O}/u-boot.bin ${O}/u-boot.bin.sd.bin ${O}/u-boot.bin.usb

${TMP}/dvinit-params.bin: device_acs.bin
	dd if=/dev/zero of=$@ bs=$(DVINIT_PARAMS_SIZE) count=1 status=none
	dd if=$< of=$@ conv=notrunc status=none

${TMP}/bb1st.sto.bin: bb1st.sto.bin.signed ${TMP}/dvinit-params.bin
	$(QEMU) $(ACPU_IMAGETOOL) create-boot-blobs \
		--infile-template-bb1st=bb1st.sto.bin.signed \
		--infile-dvinit-params=${TMP}/dvinit-params.bin \
		--scs-family=s7 \
		--outfile-bb1st=$@

${TMP}/bb1st.usb.bin: bb1st.usb.bin.signed ${TMP}/dvinit-params.bin
	$(QEMU) $(ACPU_IMAGETOOL) create-boot-blobs \
		--infile-template-bb1st=bb1st.usb.bin.signed \
		--infile-dvinit-params=${TMP}/dvinit-params.bin \
		--scs-family=s7 \
		--outfile-bb1st=$@

${TMP}/bl30-payload.bin: bl30.bin
	dd if=/dev/zero of=$@ bs=$(BL30_PAYLOAD_SIZE) count=1 status=none
	dd if=$< of=$@ conv=notrunc status=none

${TMP}/bl33-payload.bin: ${BL33}
	$(QEMU) $(AML_ENCRYPT) --bl3sig --input ${BL33} --output ${TMP}/bl33.lz4 --compress lz4 --level v3 --type bl33
	dd if=${TMP}/bl33.lz4 of=${TMP}/bl33.bin bs=1 skip=1824 status=none
	dd if=/dev/zero of=$@ bs=$(BL33_PAYLOAD_SIZE) count=1 status=none
	dd if=${TMP}/bl33.bin of=$@ conv=notrunc status=none
	@rm -f ${TMP}/bl33.lz4 ${TMP}/bl33.bin

${TMP}/device-fip.bin: device-fip-header.bin ${TMP}/bl30-payload.bin ${TMP}/bl33-payload.bin \
                       blob-bl31.bin.signed blob-bl32.bin.signed blob-bl40.bin.signed
	$(QEMU) $(ACPU_IMAGETOOL) create-device-fip \
		--infile-template-chipset-fip-header=device-fip-header.bin \
		--infile-bl30-payload=${TMP}/bl30-payload.bin \
		--infile-bl33-payload=${TMP}/bl33-payload.bin \
		--infile-blob-bl40=blob-bl40.bin.signed \
		--infile-blob-bl31=blob-bl31.bin.signed \
		--infile-blob-bl32=blob-bl32.bin.signed \
		--outfile-device-fip=$@ \
		--header-layout=full

${O}/u-boot.bin: ${TMP}/bb1st.sto.bin ${TMP}/bb1st.usb.bin \
                 blob-bl2e.sto.bin.signed blob-bl2e.usb.bin.signed blob-bl2x.bin.signed \
                 ${TMP}/device-fip.bin
	@# Prepare DDR FIP (use board's or create empty 256KB)
	@if [ -f ddr-fip.bin ]; then \
		cp ddr-fip.bin ${TMP}/ddr-fip.bin; \
	else \
		dd if=/dev/zero of=${TMP}/ddr-fip.bin bs=1024 count=256 status=none; \
	fi
	@# ---- Build storage (STO) bootloader ----
	@set -e; \
	ALIGN=4096; SECTOR=512; \
	bb1st=${TMP}/bb1st.sto.bin; \
	bl2e=blob-bl2e.sto.bin.signed; \
	bl2x=blob-bl2x.bin.signed; \
	ddr_fip=${TMP}/ddr-fip.bin; \
	devfip=${TMP}/device-fip.bin; \
	bootloader=${O}/u-boot.bin; \
	sdimage=${O}/u-boot.bin.sd.bin; \
	total=0; \
	for f in $$bb1st $$bl2e $$bl2x $$ddr_fip $$devfip; do \
		sz=$$(stat -c %s $$f); \
		aligned=$$(( ((sz + ALIGN - 1) / ALIGN) * ALIGN )); \
		total=$$((total + aligned)); \
	done; \
	dd if=/dev/zero of=$$bootloader bs=$$total count=1 status=none; \
	seek_sector=0; \
	cfg=${TMP}/aml-payload.cfg; \
	printf '@AMLBOOT' > $$cfg; \
	printf '02059000' | xxd -r -ps >> $$cfg; \
	printf 'S7-s905y5-%s' "$$(date +%y%m%d%H%M%S)" | dd bs=1 count=20 status=none >> $$cfg 2>/dev/null; \
	idx=0; \
	for f in $$bb1st $$bl2e $$bl2x $$ddr_fip $$devfip; do \
		sz=$$(stat -c %s $$f); \
		aligned=$$(( ((sz + ALIGN - 1) / ALIGN) * ALIGN )); \
		dd if=$$f of=$$bootloader bs=$$SECTOR seek=$$seek_sector conv=notrunc status=none; \
		ofs=$$((seek_sector * SECTOR)); \
		case $$idx in \
			0) printf 'BBST' >> $$cfg ;; \
			1) printf 'BL2E' >> $$cfg ;; \
			2) printf 'BL2X' >> $$cfg ;; \
			3) printf 'DDRF' >> $$cfg ;; \
			4) printf 'DEVF' >> $$cfg ;; \
		esac; \
		printf '%02x%02x%02x%02x' $$((ofs & 0xFF)) $$(((ofs >> 8) & 0xFF)) $$(((ofs >> 16) & 0xFF)) $$(((ofs >> 24) & 0xFF)) | xxd -r -ps >> $$cfg; \
		printf '%02x%02x%02x%02x' $$((aligned & 0xFF)) $$(((aligned >> 8) & 0xFF)) $$(((aligned >> 16) & 0xFF)) $$(((aligned >> 24) & 0xFF)) | xxd -r -ps >> $$cfg; \
		printf '00000000' | xxd -r -ps >> $$cfg; \
		seek_sector=$$((seek_sector + aligned / SECTOR)); \
		idx=$$((idx + 1)); \
	done; \
	openssl dgst -sha256 -binary $$cfg > $$cfg.sha256; \
	cat $$cfg >> $$cfg.sha256; \
	mv -f $$cfg.sha256 $$cfg; \
	dd if=$$cfg of=$$bootloader bs=512 seek=404 conv=notrunc status=none; \
	total_sd=$$((total + SECTOR)); \
	dd if=/dev/zero of=$$sdimage bs=$$total_sd count=1 status=none; \
	dd if=$$cfg of=$$sdimage conv=notrunc status=none; \
	dd if=$$bootloader of=$$sdimage bs=512 seek=1 conv=notrunc status=none; \
	rm -f $$cfg
	@# ---- Build USB bootloader ----
	@set -e; \
	ALIGN=4096; SECTOR=512; \
	bb1st=${TMP}/bb1st.usb.bin; \
	bl2e=blob-bl2e.usb.bin.signed; \
	bl2x=blob-bl2x.bin.signed; \
	ddr_fip=${TMP}/ddr-fip.bin; \
	devfip=${TMP}/device-fip.bin; \
	bootloader=${O}/u-boot.bin.usb; \
	total=0; \
	for f in $$bb1st $$bl2e $$bl2x $$ddr_fip $$devfip; do \
		sz=$$(stat -c %s $$f); \
		aligned=$$(( ((sz + ALIGN - 1) / ALIGN) * ALIGN )); \
		total=$$((total + aligned)); \
	done; \
	dd if=/dev/zero of=$$bootloader bs=$$total count=1 status=none; \
	seek_sector=0; \
	cfg=${TMP}/aml-payload.cfg; \
	printf '@AMLBOOT' > $$cfg; \
	printf '02059000' | xxd -r -ps >> $$cfg; \
	printf 'S7-s905y5-%s' "$$(date +%y%m%d%H%M%S)" | dd bs=1 count=20 status=none >> $$cfg 2>/dev/null; \
	idx=0; \
	for f in $$bb1st $$bl2e $$bl2x $$ddr_fip $$devfip; do \
		sz=$$(stat -c %s $$f); \
		aligned=$$(( ((sz + ALIGN - 1) / ALIGN) * ALIGN )); \
		dd if=$$f of=$$bootloader bs=$$SECTOR seek=$$seek_sector conv=notrunc status=none; \
		ofs=$$((seek_sector * SECTOR)); \
		case $$idx in \
			0) printf 'BBST' >> $$cfg ;; \
			1) printf 'BL2E' >> $$cfg ;; \
			2) printf 'BL2X' >> $$cfg ;; \
			3) printf 'DDRF' >> $$cfg ;; \
			4) printf 'DEVF' >> $$cfg ;; \
		esac; \
		printf '%02x%02x%02x%02x' $$((ofs & 0xFF)) $$(((ofs >> 8) & 0xFF)) $$(((ofs >> 16) & 0xFF)) $$(((ofs >> 24) & 0xFF)) | xxd -r -ps >> $$cfg; \
		printf '%02x%02x%02x%02x' $$((aligned & 0xFF)) $$(((aligned >> 8) & 0xFF)) $$(((aligned >> 16) & 0xFF)) $$(((aligned >> 24) & 0xFF)) | xxd -r -ps >> $$cfg; \
		printf '00000000' | xxd -r -ps >> $$cfg; \
		seek_sector=$$((seek_sector + aligned / SECTOR)); \
		idx=$$((idx + 1)); \
	done; \
	openssl dgst -sha256 -binary $$cfg > $$cfg.sha256; \
	cat $$cfg >> $$cfg.sha256; \
	mv -f $$cfg.sha256 $$cfg; \
	dd if=$$cfg of=$$bootloader bs=512 seek=404 conv=notrunc status=none; \
	rm -f $$cfg
