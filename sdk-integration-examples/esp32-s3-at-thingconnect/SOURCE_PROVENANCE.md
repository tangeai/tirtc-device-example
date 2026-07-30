# Source provenance

## Published identity

- Project: TiRTC ESP32-S3 AT ThingConnect Demo
- Version: `0.2.0`
- Published path: `sdk-integration-examples/esp32-s3-at-thingconnect`
- Public tag: `esp32-s3-at-thingconnect-v0.2.0`

The development handoff was a standalone source-tree snapshot without an
upstream Git repository, tag, or commit. No source tag or commit is invented
for that handoff. Once published, the commit referenced by the public tag above
is the authoritative source identity. The accompanying GitHub Release manifest
records that full commit and the SHA-256 of every published source file and
firmware asset.

## Imported scope

Included:

- ESP-IDF project files, `main/`, and application components.
- TiRTC SDK headers, ESP32-S3 archive, version, and build manifest.
- AT, architecture, validation, user, and experience documentation.
- Recorded G711A/H264 test media and their profile.
- AT regression, scenario, capture, and static validation tools.

Excluded:

- All development `build*` directories and generated `sdkconfig`.
- Serial evidence under `artifacts/`.
- Python caches, editor state, logs, and local release staging.
- Wi-Fi credentials, bound-device identities, keys, and short-lived tokens.

## Dependency identity

- ESP-IDF: `v5.5.4`
- TiRTC SDK: `v2.2.1`
- TiRTC SDK commit: `3a33bf4ae51b3ab9eb246648adb274d0fae32ebf`
- TiRTC archive SHA-256:
  `dc9f869194584fd43fe091f887342d170a1422e657ce1fc2a144c90a3ade1f8e`

Build success, target flashing, platform binding, AI, device calls, race tests,
and real-media validation remain separate evidence layers. The release
manifest and release notes state which layers were completed for this version.
