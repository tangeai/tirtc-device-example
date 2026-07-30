# Version

- Example: `0.2.0`
- Target: ESP32-S3
- ESP-IDF: `v5.5.4`
- TiRTC SDK: `v2.2.1`
- TiRTC SDK commit: `3a33bf4ae51b3ab9eb246648adb274d0fae32ebf`
- TiRTC SDK BuildInfo:
  `{"tirtc":"v2.2.1","commit":"3a33bf4ae51b","tgtrp":"v1.4.6"}`
- TiRTC archive SHA256:
  `dc9f869194584fd43fe091f887342d170a1422e657ce1fc2a144c90a3ade1f8e`
- AT response schemas: `BUILD=1`, `SESSION=3` (22 CSV fields), `MEDIA=1`
- Hardware evidence summary schema: `4`
  - every JSONL record uses one random `run_id`
  - `summary.json.raw_evidence` records the same `run_id`, raw JSONL path,
    size, record count and SHA256

`0.2.0` is the formal source version. The GitHub Release manifest binds the
published source commit to its firmware assets. These version fields identify
source and evidence formats; they do not by themselves claim hardware
certification or replace separate flashing, platform binding, AI, call, race,
and real-media evidence.
