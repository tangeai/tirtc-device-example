#!/usr/bin/env bash
set -euo pipefail

: "${SDK_ROOT:?Set SDK_ROOT to im_sdk/opensource/freertos}"
: "${TOOLCHAIN_BIN:?Set TOOLCHAIN_BIN to the G32S10X toolchain bin directory}"

JOBS="${JOBS:-4}"
LOCAL_BIN="${LOCAL_BIN:-${HOME}/.local/bin}"
DEMO_NAME="tirtc_g32s10x_wifi_link_demo"
DEMO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET_APP="${SDK_ROOT}/application/${DEMO_NAME}"

test -f "${SDK_ROOT}/Makefile"
test -x "${TOOLCHAIN_BIN}/riscv32-unknown-elf-gcc"
test -f "${DEMO_ROOT}/sdk/lib/g32/libTiRTC.a"

if [[ -e "${TARGET_APP}" ]]; then
  echo "Refusing to overwrite existing SDK application: ${TARGET_APP}" >&2
  echo "Use a clean SDK tree or remove the old integration deliberately." >&2
  exit 2
fi

cp -a "${DEMO_ROOT}" "${TARGET_APP}"
cp -a "${DEMO_ROOT}/port/g32/vendor_overrides/." "${SDK_ROOT}/"
cp "${DEMO_ROOT}/integration/application/application.c" \
  "${SDK_ROOT}/application/application.c"
cp "${DEMO_ROOT}/integration/package/application/Config.in" \
  "${SDK_ROOT}/package/application/Config.in"
cp "${DEMO_ROOT}/integration/package/application/application.mk" \
  "${SDK_ROOT}/package/application/application.mk"
cp -a "${DEMO_ROOT}/integration/package/application/${DEMO_NAME}" \
  "${SDK_ROOT}/package/application/${DEMO_NAME}"
cp "${DEMO_ROOT}/integration/configs/g32s10x_tirtc_wifi_link_demo_defconfig" \
  "${SDK_ROOT}/configs/g32s10x_tirtc_wifi_link_demo_defconfig"

export PATH="${TOOLCHAIN_BIN}:${LOCAL_BIN}:${PATH}"
make -C "${SDK_ROOT}" g32s10x_tirtc_wifi_link_demo_defconfig
make -C "${SDK_ROOT}" -j"${JOBS}"

echo "Build finished. Check ${SDK_ROOT}/rtos-with-spl.bin and build logs."
