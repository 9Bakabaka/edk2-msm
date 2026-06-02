function platform_build_kernel(){
	local UEFI_FD="${WORKSPACE}/Build/${DEVICE}/${_MODE}_${TOOLCHAIN}/FV/${SOC_PLATFORM}_UEFI.fd"
	if ! [ -f "${UEFI_FD}" ]
	then
		typeset -u SOC_PLATFORM_U="${SOC_PLATFORM}"
		UEFI_FD="${WORKSPACE}/Build/${DEVICE}/${_MODE}_${TOOLCHAIN}/FV/${SOC_PLATFORM_U}_UEFI.fd"
	fi

	cat \
		"${ROOTDIR}/tools/BootShim/BootShim.bin" \
		"${UEFI_FD}" \
		> "${WORKSPACE}/Build/${DEVICE}/${_MODE}_${TOOLCHAIN}/FV/${SOC_PLATFORM}_UEFI.fd-bootshim" \
		||return "$?"
	gzip -c \
		< "${WORKSPACE}/Build/${DEVICE}/${_MODE}_${TOOLCHAIN}/FV/${SOC_PLATFORM}_UEFI.fd-bootshim" \
		> "${WORKSPACE}/uefi-${DEVICE}.img.gz" \
		||return "$?"
	cat \
		"${WORKSPACE}/uefi-${DEVICE}.img.gz" \
		"${ROOTDIR}/Platform/${VENDOR_NAME}/${SOC_PLATFORM_L}/FdtBlob_compat/${PLATFORM_NAME}.dtb" \
		> "${WORKSPACE}/uefi-${DEVICE}.img.gz-dtb" \
		||return "$?"
	cp "${WORKSPACE}/uefi-${DEVICE}.img.gz-dtb" "${WORKSPACE}/uefi-${DEVICE}-kernel"
}

function platform_build_bootimg(){
		python3 "${ROOTDIR}/tools/mkbootimg.py" \
		--kernel "${WORKSPACE}/uefi-${DEVICE}-kernel" \
		--ramdisk ramdisk \
		--kernel_offset 0x00000000 \
		--ramdisk_offset 0x00000000 \
		--tags_offset 0x00000000 \
		--os_version "${BOOTIMG_OS_VERSION}" \
		--os_patch_level "${BOOTIMG_OS_PATCH_LEVEL}" \
		--header_version "${BOOTIMG_HEADER_VERSION}" \
		-o "${OUTDIR}/boot-${DEVICE}${EXT}.img" \
		||return "$?"
}
