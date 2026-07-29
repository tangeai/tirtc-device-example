#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sdk_root="${1:-${G32_SDK_ROOT:-}}"

if [[ -z "${sdk_root}" ]]; then
    echo "set G32_SDK_ROOT or pass sdk_root as the first argument" >&2
    exit 2
fi
if [[ ! -f "${sdk_root}/Makefile" || ! -d "${sdk_root}/application" ]]; then
    echo "invalid Ingenic FreeRTOS SDK root: ${sdk_root}" >&2
    exit 2
fi

mkdir -p "${sdk_root}/application/app_tirtc_demo"
cp -a "${repo_root}/app/." "${sdk_root}/application/app_tirtc_demo/"
rm -f \
    "${sdk_root}/application/app_tirtc_demo/builtin.o" \
    "${sdk_root}/application/app_tirtc_demo/src/tirtc_demo_cloud.c.orig"

mkdir -p "${sdk_root}/third_party/tirtc"
cp -a "${repo_root}/sdk/." "${sdk_root}/third_party/tirtc/"

mkdir -p \
    "${sdk_root}/package/application/app_tirtc_demo" \
    "${sdk_root}/package/third_party/tirtc"
cp -a \
    "${repo_root}/integration/package/application/app_tirtc_demo/." \
    "${sdk_root}/package/application/app_tirtc_demo/"
cp -a \
    "${repo_root}/integration/package/third_party/tirtc/." \
    "${sdk_root}/package/third_party/tirtc/"
cp \
    "${repo_root}/integration/package/application/Config.in" \
    "${sdk_root}/package/application/Config.in"
cp \
    "${repo_root}/integration/package/application/application.mk" \
    "${sdk_root}/package/application/application.mk"
cp \
    "${repo_root}/integration/package/third_party/Config.in" \
    "${sdk_root}/package/third_party/Config.in"
cp \
    "${repo_root}/integration/package/third_party/third_party.mk" \
    "${sdk_root}/package/third_party/third_party.mk"

cp -a "${repo_root}/integration/vendor_overrides/." "${sdk_root}/"
cp \
    "${repo_root}/integration/configs/g32s10x_tirtc_app_release_defconfig" \
    "${sdk_root}/configs/g32s10x_tirtc_app_release_defconfig"

echo "APP overlay installed: ${sdk_root}"
