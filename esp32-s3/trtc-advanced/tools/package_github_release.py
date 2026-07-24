#!/usr/bin/env python3
"""Build GitHub Release assets from an existing ESP-IDF build directory."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import zipfile
from datetime import datetime, timezone
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def git_output(project: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(project), *args],
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    return result.stdout.strip()


def write_zip(archive: Path, base: Path, files: list[Path]) -> None:
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as output:
        for path in files:
            # Release archives must be reproducible. File mtimes otherwise make
            # the zip hash change even when every firmware byte is identical.
            info = zipfile.ZipInfo(
                filename=path.relative_to(base).as_posix(),
                date_time=(1980, 1, 1, 0, 0, 0),
            )
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            output.writestr(info, path.read_bytes(), compresslevel=9)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--name", default="esp32s3-tirtc-device-monitor")
    parser.add_argument("--tag", default="")
    parser.add_argument(
        "--build-dir",
        type=Path,
        help="ESP-IDF build directory. Relative paths are resolved from --project.",
    )
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    project = args.project.resolve()
    build_arg = args.build_dir or Path("build")
    build = (build_arg if build_arg.is_absolute() else project / build_arg).resolve()
    flasher_path = build / "flasher_args.json"
    project_description_path = build / "project_description.json"
    if not flasher_path.is_file() or not project_description_path.is_file():
        raise SystemExit("build/flasher_args.json or build/project_description.json is missing")

    project_description = json.loads(project_description_path.read_text(encoding="utf-8"))
    built_version = str(project_description.get("project_version", ""))
    if built_version != args.version:
        raise SystemExit(f"version mismatch: requested={args.version} build={built_version}")

    tag = args.tag or f"esp32-s3-trtc-advanced-v{args.version}"
    output = (args.output or project / "artifacts" / "github-releases" / tag).resolve()
    if output == project or output == build:
        raise SystemExit("refusing to use the project or build root as output")
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)

    flasher = json.loads(flasher_path.read_text(encoding="utf-8"))
    flash_files = flasher["flash_files"]
    resolved_flash_files: list[tuple[str, Path]] = []
    for offset, relative in flash_files.items():
        source = build / relative
        if not source.is_file():
            raise SystemExit(f"flash input is missing: {source}")
        resolved_flash_files.append((offset, source))
    resolved_flash_files.sort(key=lambda item: int(item[0], 0))

    maintenance = output / "maintenance"
    maintenance.mkdir()
    copied: list[Path] = []
    flash_lines: list[str] = []
    for offset, source in resolved_flash_files:
        destination = maintenance / source.name
        shutil.copy2(source, destination)
        copied.append(destination)
        flash_lines.append(f"{offset} {destination.name}")

    (maintenance / "flash_args.txt").write_text(
        "\n".join(flash_lines) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    (maintenance / "README_FLASH_CN.md").write_text(
        "# 维护者多地址烧录包\n\n"
        "普通体验者请使用 Release 根目录的 0x0 完整镜像和 Espressif ESP Launchpad。\n"
        "本目录只用于维护者排障，地址以 `flash_args.txt` 为准。\n",
        encoding="utf-8",
        newline="\n",
    )
    copied.extend([maintenance / "flash_args.txt", maintenance / "README_FLASH_CN.md"])

    settings = flasher["flash_settings"]
    full_image = output / f"{args.name}-full-v{args.version}.bin"
    merge_command = [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        flasher.get("extra_esptool_args", {}).get("chip", "esp32s3"),
        "merge_bin",
        "-o",
        str(full_image),
        "--flash_mode",
        settings["flash_mode"],
        "--flash_size",
        settings["flash_size"],
        "--flash_freq",
        settings["flash_freq"],
        "--fill-flash-size",
        settings["flash_size"],
    ]
    for offset, source in resolved_flash_files:
        merge_command.extend([offset, str(source)])
    subprocess.run(merge_command, check=True, cwd=build)

    install_readme = output / "README_INSTALL_CN.md"
    install_readme.write_text(
        "# ESP Launchpad 烧录\n\n"
        f"- 固件版本：`{args.version}`\n"
        f"- 选择文件：`{full_image.name}`\n"
        "- 烧录地址：`0x0`\n"
        "- 网页工具：https://espressif.github.io/esp-launchpad/\n\n"
        "首次烧录建议先擦除闪存。烧录完成后按 RESET，按设备提示连接 2.4 GHz Wi-Fi，"
        "再到 https://mqtt-demo.tange-ai.com/ 输入设备显示的 6 位验证码。\n",
        encoding="utf-8",
        newline="\n",
    )

    web_install_zip = output / f"{args.name}-webinstall-v{args.version}.zip"
    write_zip(web_install_zip, output, [full_image, install_readme])

    web_flash_zip = output / f"{args.name}-webflash-v{args.version}.zip"
    write_zip(web_flash_zip, maintenance, copied)

    app_source = build / flasher["app"]["file"]
    ota_image = output / f"{args.name}-ota-v{args.version}.bin"
    shutil.copy2(app_source, ota_image)

    source_commit = git_output(project, "rev-parse", "HEAD")
    dirty = bool(git_output(project, "status", "--porcelain"))
    release_files = [ota_image, full_image, web_install_zip, web_flash_zip]
    manifest = {
        "schema": 1,
        "tag": tag,
        "platform": "esp32-s3",
        "example": "trtc-advanced",
        "version": args.version,
        "project": project_description.get("project_name"),
        "source_commit": source_commit,
        "source_dirty_when_packaged": dirty,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "flash": {
            "full_image_offset": "0x0",
            "mode": settings["flash_mode"],
            "size": settings["flash_size"],
            "frequency": settings["flash_freq"],
            "files": [{"offset": offset, "source": source.name} for offset, source in resolved_flash_files],
        },
        "assets": [
            {"name": path.name, "size": path.stat().st_size, "sha256": sha256(path)}
            for path in release_files
        ],
    }
    manifest_path = output / "release-manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )

    checksum_files = [*release_files, manifest_path]
    checksum_path = output / "SHA256SUMS.txt"
    checksum_path.write_text(
        "".join(f"{sha256(path)}  {path.name}\n" for path in checksum_files),
        encoding="utf-8",
        newline="\n",
    )

    print(f"release_dir={output}")
    for path in [*release_files, manifest_path, checksum_path]:
        print(f"{sha256(path)}  {path.stat().st_size:>10}  {path.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
