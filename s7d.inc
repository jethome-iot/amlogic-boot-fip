# S7D SoC build include file for amlogic-boot-fip

O ?= .
TMP ?= .
BL33 ?=

# Sizes from variable_soc.sh
BL33_PAYLOAD_SIZE := 1572864
BL30_PAYLOAD_SIZE := 65536
BL31_PAYLOAD_SIZE := 262144
BL32_PAYLOAD_SIZE := 524288
BL40_PAYLOAD_SIZE := 98304
BL2_MAX_SIZE := 272384
ALIGN_BASE := 4096

# Fixed sizes for boot blobs (from variable_soc.sh BLX_BIN_SIZE)
# These MUST match what ROM expects in @AMLBOOT table
BL2E_FIXED_SIZE := 116912
BL2X_FIXED_SIZE := 108720
# Aligned to 4K
BL2E_ALIGNED_SIZE := 118784
BL2X_ALIGNED_SIZE := 110592
DDR_FIP_SIZE := 262144

.PHONY: clean distclean all

all: ${O}/u-boot.bin ${O}/u-boot.bin.sd.bin

clean:
	rm -f ${TMP}/bl33-payload.bin
	rm -f ${TMP}/bl30-payload.bin
	rm -f ${TMP}/bl2e.padded
	rm -f ${TMP}/bl2x.padded
	rm -f ${TMP}/device-fip.bin
	rm -f ${TMP}/bb1st.bin.hdr
	rm -f ${TMP}/bb1st.bin.max
	rm -f ${TMP}/bb1st.bin.max.hdr
	rm -f ${TMP}/ddr-fip.bin
	rm -f ${TMP}/aml-payload.cfg

distclean: clean
	rm -f ${O}/u-boot.bin ${O}/u-boot.bin.sd.bin ${O}/u-boot.bin.usb.signed

# Create BL33 payload with correct padding
${TMP}/bl33-payload.bin: ${BL33}
	@echo "Creating BL33 payload from $(BL33)..."
	dd if=/dev/zero of=$@ bs=${BL33_PAYLOAD_SIZE} count=1 status=none
	dd if=${BL33} of=$@ conv=notrunc status=none

# Create BL30 payload
${TMP}/bl30-payload.bin: bl30.bin
	@echo "Creating BL30 payload..."
	dd if=/dev/zero of=$@ bs=${BL30_PAYLOAD_SIZE} count=1 status=none
	dd if=bl30.bin of=$@ conv=notrunc status=none

# Pad BL2E to fixed size
${TMP}/bl2e.padded: bl2e.bin
	@echo "Padding BL2E to fixed size ${BL2E_FIXED_SIZE}..."
	dd if=/dev/zero of=$@ bs=1 count=${BL2E_FIXED_SIZE} status=none
	dd if=bl2e.bin of=$@ conv=notrunc status=none

# Pad BL2X to fixed size
${TMP}/bl2x.padded: bl2x.bin
	@echo "Padding BL2X to fixed size ${BL2X_FIXED_SIZE}..."
	dd if=/dev/zero of=$@ bs=1 count=${BL2X_FIXED_SIZE} status=none
	dd if=bl2x.bin of=$@ conv=notrunc status=none

# Create device-fip.bin using acpu-imagetool
# Note: bl31, bl32, bl40 are pre-signed blobs, so use --infile-blob-*
# bl30 and bl33 are payloads that need padding
${TMP}/device-fip.bin: ${TMP}/bl33-payload.bin ${TMP}/bl30-payload.bin
	@echo "Creating device-fip.bin..."
	./acpu-imagetool create-device-fip \
		--infile-template-chipset-fip-header=device-fip-header.bin \
		--infile-bl30-payload=${TMP}/bl30-payload.bin \
		--infile-bl33-payload=${TMP}/bl33-payload.bin \
		--infile-blob-bl31=bl31.bin \
		--infile-blob-bl32=bl32.bin \
		--infile-blob-bl40=bl40.bin \
		--header-layout=mini \
		--chipset-authen-algorithm=rsa,none \
		--device-authen-algorithm=rsa,none \
		--outfile-device-fip=$@

# Add SBH (Secure Boot Header) to bb1st (bl2.bin)
# This is critical - ROM code expects SBH before bb1st
# First pad bb1st to BL2_MAX_SIZE (272384 = 266*1024), then add SBH header
${TMP}/bb1st.bin.hdr: bl2.bin
	@echo "Adding Secure Boot Header to bb1st..."
	dd if=/dev/zero of=${TMP}/bb1st.bin.max bs=1024 count=266 status=none
	dd if=bl2.bin of=${TMP}/bb1st.bin.max conv=notrunc status=none
	./attach_sbh.sh ${TMP}/bb1st.bin.max $@

# Create empty ddr-fip.bin (256KB placeholder)
# Real DDR firmware is embedded in bb1st for S7D
${TMP}/ddr-fip.bin:
	@echo "Creating empty ddr-fip.bin..."
	dd if=/dev/zero of=$@ bs=1024 count=256 status=none

# Create final u-boot.bin (storage version)
# Use FIXED sizes for BL2E/BL2X to match what ROM expects
${O}/u-boot.bin: ${TMP}/device-fip.bin ${TMP}/bb1st.bin.hdr ${TMP}/ddr-fip.bin ${TMP}/bl2e.padded ${TMP}/bl2x.padded
	@echo "Creating final u-boot.bin..."
	$(eval BB1ST_SIZE := $(shell stat -c %s ${TMP}/bb1st.bin.hdr))
	$(eval DEVFIP_SIZE := $(shell stat -c %s ${TMP}/device-fip.bin))
	@# Align BB1ST to 4K
	$(eval BB1ST_ALIGNED := $(shell echo $$(( ($(BB1ST_SIZE) + 4095) / 4096 * 4096 )) ))
	$(eval DEVFIP_ALIGNED := $(shell echo $$(( ($(DEVFIP_SIZE) + 4095) / 4096 * 4096 )) ))
	@# Calculate offsets using FIXED sizes
	$(eval OFF_BB1ST := 0)
	$(eval OFF_BL2E := $(BB1ST_ALIGNED))
	$(eval OFF_BL2X := $(shell echo $$(( $(OFF_BL2E) + $(BL2E_ALIGNED_SIZE) )) ))
	$(eval OFF_DDR := $(shell echo $$(( $(OFF_BL2X) + $(BL2X_ALIGNED_SIZE) )) ))
	$(eval OFF_DEVF := $(shell echo $$(( $(OFF_DDR) + $(DDR_FIP_SIZE) )) ))
	$(eval TOTAL_SIZE := $(shell echo $$(( $(OFF_DEVF) + $(DEVFIP_ALIGNED) )) ))
	@echo "Sizes: BB1ST=$(BB1ST_SIZE) BL2E=$(BL2E_ALIGNED_SIZE) BL2X=$(BL2X_ALIGNED_SIZE) DDR=$(DDR_FIP_SIZE) DEVFIP=$(DEVFIP_SIZE)"
	@echo "Aligned: BB1ST=$(BB1ST_ALIGNED) BL2E=$(BL2E_ALIGNED_SIZE) BL2X=$(BL2X_ALIGNED_SIZE) DDR=$(DDR_FIP_SIZE) DEVFIP=$(DEVFIP_ALIGNED)"
	@echo "Offsets: BB1ST=0x$$(printf '%x' $(OFF_BB1ST)) BL2E=0x$$(printf '%x' $(OFF_BL2E)) BL2X=0x$$(printf '%x' $(OFF_BL2X)) DDR=0x$$(printf '%x' $(OFF_DDR)) DEVF=0x$$(printf '%x' $(OFF_DEVF))"
	@echo "Total image size: $(TOTAL_SIZE) bytes"
	@# Create empty file
	dd if=/dev/zero of=$@ bs=$(TOTAL_SIZE) count=1 status=none
	@# Write components at correct offsets using padded files
	dd if=${TMP}/bb1st.bin.hdr of=$@ bs=1 seek=$(OFF_BB1ST) conv=notrunc status=none
	dd if=${TMP}/bl2e.padded of=$@ bs=1 seek=$(OFF_BL2E) conv=notrunc status=none
	dd if=${TMP}/bl2x.padded of=$@ bs=1 seek=$(OFF_BL2X) conv=notrunc status=none
	dd if=${TMP}/ddr-fip.bin of=$@ bs=1 seek=$(OFF_DDR) conv=notrunc status=none
	dd if=${TMP}/device-fip.bin of=$@ bs=1 seek=$(OFF_DEVF) conv=notrunc status=none
	@# Generate payload config (@AMLBOOT table)
	@echo "Generating @AMLBOOT payload config..."
	@rm -f ${TMP}/aml-payload.cfg
	@# Header: @AMLBOOT + Version(02) + nItemNum(05) + nSizeHDR(0x90=144)
	@/bin/echo -n -e '@AMLBOOT\x02\x05\x90\x00' > ${TMP}/aml-payload.cfg
	@# Date stamp (20 bytes) - S7D-s905x5m-YYMMDDHHMM + padding
	@/bin/echo -n "S7D-s905x5m-$$(date +%y%m%d%H%M)" >> ${TMP}/aml-payload.cfg
	@truncate -s 32 ${TMP}/aml-payload.cfg
	@# BBST entry (name + offset_le32 + size_le32 + reserved)
	@/bin/echo -n 'BBST' >> ${TMP}/aml-payload.cfg
	@printf '%08x' $(OFF_BB1ST) | sed 's/\(..\)/\1\n/g' | tac | tr -d '\n' | xxd -r -p >> ${TMP}/aml-payload.cfg
	@printf '%08x' $(BB1ST_ALIGNED) | sed 's/\(..\)/\1\n/g' | tac | tr -d '\n' | xxd -r -p >> ${TMP}/aml-payload.cfg
	@/bin/echo -n -e '\x00\x00\x00\x00' >> ${TMP}/aml-payload.cfg
	@# BL2E entry - use FIXED aligned size
	@/bin/echo -n 'BL2E' >> ${TMP}/aml-payload.cfg
	@printf '%08x' $(OFF_BL2E) | sed 's/\(..\)/\1\n/g' | tac | tr -d '\n' | xxd -r -p >> ${TMP}/aml-payload.cfg
	@printf '%08x' $(BL2E_ALIGNED_SIZE) | sed 's/\(..\)/\1\n/g' | tac | tr -d '\n' | xxd -r -p >> ${TMP}/aml-payload.cfg
	@/bin/echo -n -e '\x00\x00\x00\x00' >> ${TMP}/aml-payload.cfg
	@# BL2X entry - use FIXED aligned size
	@/bin/echo -n 'BL2X' >> ${TMP}/aml-payload.cfg
	@printf '%08x' $(OFF_BL2X) | sed 's/\(..\)/\1\n/g' | tac | tr -d '\n' | xxd -r -p >> ${TMP}/aml-payload.cfg
	@printf '%08x' $(BL2X_ALIGNED_SIZE) | sed 's/\(..\)/\1\n/g' | tac | tr -d '\n' | xxd -r -p >> ${TMP}/aml-payload.cfg
	@/bin/echo -n -e '\x00\x00\x00\x00' >> ${TMP}/aml-payload.cfg
	@# DDRF entry - use FIXED size
	@/bin/echo -n 'DDRF' >> ${TMP}/aml-payload.cfg
	@printf '%08x' $(OFF_DDR) | sed 's/\(..\)/\1\n/g' | tac | tr -d '\n' | xxd -r -p >> ${TMP}/aml-payload.cfg
	@printf '%08x' $(DDR_FIP_SIZE) | sed 's/\(..\)/\1\n/g' | tac | tr -d '\n' | xxd -r -p >> ${TMP}/aml-payload.cfg
	@/bin/echo -n -e '\x00\x00\x00\x00' >> ${TMP}/aml-payload.cfg
	@# DEVF entry
	@/bin/echo -n 'DEVF' >> ${TMP}/aml-payload.cfg
	@printf '%08x' $(OFF_DEVF) | sed 's/\(..\)/\1\n/g' | tac | tr -d '\n' | xxd -r -p >> ${TMP}/aml-payload.cfg
	@printf '%08x' $(DEVFIP_ALIGNED) | sed 's/\(..\)/\1\n/g' | tac | tr -d '\n' | xxd -r -p >> ${TMP}/aml-payload.cfg
	@/bin/echo -n -e '\x00\x00\x00\x00' >> ${TMP}/aml-payload.cfg
	@# Add SHA256 hash at the beginning
	openssl dgst -sha256 -binary ${TMP}/aml-payload.cfg > ${TMP}/aml-payload.cfg.sha256
	cat ${TMP}/aml-payload.cfg >> ${TMP}/aml-payload.cfg.sha256
	mv ${TMP}/aml-payload.cfg.sha256 ${TMP}/aml-payload.cfg
	@# Write payload config at sector 540 and header at sector 543
	dd if=${TMP}/aml-payload.cfg of=$@ bs=512 seek=540 conv=notrunc status=none
	dd if=hdr_revA of=$@ bs=512 seek=543 conv=notrunc status=none
	@echo "Created: $@"

# Create SD card image (u-boot.bin with 512-byte offset for MBR)
${O}/u-boot.bin.sd.bin: ${O}/u-boot.bin
	@echo "Creating SD card image..."
	$(eval UBOOT_SIZE := $(shell stat -c %s ${O}/u-boot.bin))
	$(eval SD_SIZE := $(shell echo $$(( $(UBOOT_SIZE) + 512 )) ))
	dd if=/dev/zero of=$@ bs=$(SD_SIZE) count=1 status=none
	dd if=${O}/u-boot.bin of=$@ bs=512 seek=1 conv=notrunc status=none
	@echo "Created: $@"
