#!/usr/bin/env bash
set -euo pipefail

: "${G32_SDK_ROOT:?Set G32_SDK_ROOT to im_sdk/opensource/freertos}"
: "${G32_TOOLCHAIN_BIN:?Set G32_TOOLCHAIN_BIN to the G32S10X toolchain bin directory}"

JOBS="${JOBS:-4}"
LOCAL_BIN="${LOCAL_BIN:-${HOME}/.local/bin}"
DEMO_NAME="tirtc_g32s10x_wifi_link_demo"
DEMO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET_APP="${G32_SDK_ROOT}/application/${DEMO_NAME}"

test -f "${G32_SDK_ROOT}/Makefile"
test -x "${G32_TOOLCHAIN_BIN}/riscv32-unknown-elf-gcc"
test -f "${DEMO_ROOT}/sdk/lib/g32/libTiRTC.a"

if [[ -e "${TARGET_APP}" ]]; then
  echo "Refusing to overwrite existing SDK application: ${TARGET_APP}" >&2
  echo "Use a clean SDK tree or remove the old integration deliberately." >&2
  exit 2
fi

cp -a "${DEMO_ROOT}" "${TARGET_APP}"
cp -a "${DEMO_ROOT}/port/g32/vendor_overrides/." "${G32_SDK_ROOT}/"
cp "${DEMO_ROOT}/integration/application/application.c" \
  "${G32_SDK_ROOT}/application/application.c"
cp "${DEMO_ROOT}/integration/package/application/Config.in" \
  "${G32_SDK_ROOT}/package/application/Config.in"
cp "${DEMO_ROOT}/integration/package/application/application.mk" \
  "${G32_SDK_ROOT}/package/application/application.mk"
cp -a "${DEMO_ROOT}/integration/package/application/${DEMO_NAME}" \
  "${G32_SDK_ROOT}/package/application/${DEMO_NAME}"
cp "${DEMO_ROOT}/integration/configs/g32s10x_tirtc_wifi_link_demo_defconfig" \
  "${G32_SDK_ROOT}/configs/g32s10x_tirtc_wifi_link_demo_defconfig"

export PATH="${G32_TOOLCHAIN_BIN}:${LOCAL_BIN}:${PATH}"
make -C "${G32_SDK_ROOT}" g32s10x_tirtc_wifi_link_demo_defconfig
make -C "${G32_SDK_ROOT}" -j"${JOBS}"

echo "Build finished. Check ${G32_SDK_ROOT}/rtos-with-spl.bin and build logs."
