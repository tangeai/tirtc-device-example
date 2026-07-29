#!/usr/bin/env bash
set -euo pipefail

mode="${1:-all}"
sdk_root="${2:-${G32_SDK_ROOT:-}}"
toolchain="${3:-${G32_TOOLCHAIN_BIN:-}}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
building="${repo_root}/building"

if [[ -z "${sdk_root}" ]]; then
    echo "set G32_SDK_ROOT or pass sdk_root as the second argument" >&2
    exit 2
fi
if [[ -z "${toolchain}" ]]; then
    echo "set G32_TOOLCHAIN_BIN or pass toolchain_bin as the third argument" >&2
    exit 2
fi
if [[ ! -f "${sdk_root}/Makefile" || ! -d "${sdk_root}/application" ]]; then
    echo "invalid Ingenic FreeRTOS SDK root: ${sdk_root}" >&2
    exit 2
fi
if [[ ! -x "${toolchain}/riscv32-unknown-elf-gcc" ]]; then
    echo "G32 compiler not found in toolchain bin: ${toolchain}" >&2
    exit 2
fi

export PATH="${toolchain}:${PATH}"

check_release_config() {
    grep -qx 'CONFIG_APPLICATION_TIRTC_DEMO=y' "${sdk_root}/.config.in"
    grep -qx 'CONFIG_TIRTC=y' "${sdk_root}/.config.in"
    grep -qx 'CONFIG_WIRELESS_ATBM=y' "${sdk_root}/.config.in"
    if grep -qx 'CONFIG_APPLICATION_TIRTC_SCREEN_DEBUG=y' "${sdk_root}/.config.in"; then
        echo "release build unexpectedly enables screen debug" >&2
        exit 3
    fi
}

prepare() {
    bash "${repo_root}/scripts/install_app_overlay.sh" "${sdk_root}"
    cd "${sdk_root}"
    make g32s10x_tirtc_app_release_defconfig
    check_release_config
}

clean_build() {
    mkdir -p "${building}"
    : > "${building}/build.log"
    cd "${sdk_root}"
    make clean 2>&1 | tee -a "${building}/build.log"
}

build_firmware() {
    check_release_config
    mkdir -p "${building}"
    cd "${sdk_root}"
    make -j"${JOBS:-$(nproc)}" 2>&1 | tee -a "${building}/build.log"
}

build_filesystems() {
    check_release_config
    cd "${sdk_root}"
    make yaffs2 2>&1 | tee "${building}/fs-build.log"
    make yaffs2_data 2>&1 | tee "${building}/data-build.log"
    chmod a+r "${sdk_root}/fs.yaffs2" "${sdk_root}/data.yaffs2"
}

collect() {
    mkdir -p "${building}"
    for artifact in zero.elf zero.bin rtos-with-spl.bin fs.yaffs2 data.yaffs2; do
        if [[ ! -f "${sdk_root}/${artifact}" ]]; then
            echo "missing build artifact: ${artifact}" >&2
            exit 4
        fi
        cp "${sdk_root}/${artifact}" "${building}/${artifact}"
    done
    cp "${sdk_root}/.config.in" "${building}/config.in"
    if [[ -f "${sdk_root}/include/config.h" ]]; then
        cp "${sdk_root}/include/config.h" "${building}/config.h"
    fi

    (
        cd "${building}"
        sha256sum zero.elf zero.bin rtos-with-spl.bin fs.yaffs2 data.yaffs2 > SHA256SUMS.txt
    )
    {
        echo "app_version=$(tr -d '\r\n' < "${repo_root}/VERSION")"
        echo "sdk_version=2.2.1"
        echo "sdk_build=3a33bf4ae51b"
        echo "sdk_lib_sha256=33e889d70d4459587faf57f611e9648d52accb2bc02d0ec75205a52e9ca27fba"
        echo "screen_debug=disabled"
        echo "built_at=$(date --iso-8601=seconds)"
        echo "toolchain=$(riscv32-unknown-elf-gcc -dumpfullversion)"
    } > "${building}/BUILD-METADATA.txt"

    strings "${building}/zero.elf" > "${building}/zero.strings.tmp"
    grep -F '[tirtc_demo] app version=%s' "${building}/zero.strings.tmp" >/dev/null
    grep -Fx "$(tr -d '\r\n' < "${repo_root}/VERSION")" \
        "${building}/zero.strings.tmp" >/dev/null
    rm -f "${building}/zero.strings.tmp"
    echo "release artifacts collected: ${building}"
}

case "${mode}" in
    prepare)
        prepare
        ;;
    preflight)
        prepare
        bash "${repo_root}/scripts/static_compile.sh" "${sdk_root}" "${toolchain}"
        ;;
    clean)
        clean_build
        ;;
    firmware)
        build_firmware
        ;;
    filesystems)
        build_filesystems
        ;;
    collect)
        collect
        ;;
    all)
        prepare
        bash "${repo_root}/scripts/static_compile.sh" "${sdk_root}" "${toolchain}"
        clean_build
        build_firmware
        build_filesystems
        collect
        ;;
    *)
        echo "usage: $0 [prepare|preflight|clean|firmware|filesystems|collect|all] [sdk_root] [toolchain_bin]" >&2
        exit 2
        ;;
esac
