#!/usr/bin/env bash
set -euo pipefail

sdk_root="${1:-${G32_SDK_ROOT:-}}"
toolchain="${2:-${G32_TOOLCHAIN_BIN:-}}"

if [[ -z "${sdk_root}" ]]; then
    echo "set G32_SDK_ROOT or pass sdk_root as the first argument" >&2
    exit 2
fi
if [[ -z "${toolchain}" ]]; then
    echo "set G32_TOOLCHAIN_BIN or pass toolchain_bin as the second argument" >&2
    exit 2
fi
if [[ ! -x "${toolchain}/riscv32-unknown-elf-gcc" ]]; then
    echo "G32 compiler not found in toolchain bin: ${toolchain}" >&2
    exit 2
fi

export PATH="${toolchain}:${PATH}"

cd "${sdk_root}"
flags="$(make c_compile_flags | sed -n '/^-include /p' | tail -n 1)"
if [[ -z "${flags}" ]]; then
    echo "failed to obtain Ingenic compiler flags" >&2
    exit 2
fi
read -r -a cflags <<< "${flags}"

units=(
    application/app_tirtc_demo/src/tirtc_demo_app.c
    application/app_tirtc_demo/src/tirtc_demo_cloud.c
    application/app_tirtc_demo/src/tirtc_demo_media.c
    application/app_tirtc_demo/src/tirtc_demo_platform.c
    application/app_tirtc_demo/src/tirtc_demo_sdk_gate.c
    application/app_tirtc_demo/ui/ui_tirtc_demo.c
    application/application.c
    application/com_services/audio/cap/src/audio_cap.c
    application/com_services/video/src/video_cap.c
    application/com_services/time/time_service.c
    riscv1/soc_g32s10x/jpeg/jpege_encoder.c
    newlib_riscv/gettimeofday.c
)

for unit in "${units[@]}"; do
    echo "STATIC_CHECK ${unit}"
    warning_policy=(
        -Werror
        -Wno-error=sign-compare
        -Wno-error=undef
    )
    if [[ "${unit}" != application/app_tirtc_demo/* ]]; then
        warning_policy+=(
            -Wno-error=shadow
            -Wno-error=unused-parameter
        )
    fi
    riscv32-unknown-elf-gcc \
        "${cflags[@]}" \
        -Iapplication/com_services/audio/cap/3A \
        -Wextra -Wshadow -Wformat=2 -Wundef -Wstack-usage=8192 \
        "${warning_policy[@]}" \
        -fsyntax-only "${unit}"
done

echo "G32 APP strict static compile: PASS (${#units[@]} units)"
