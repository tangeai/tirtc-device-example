# Test media

These files are synthetic protocol fixtures generated for this example. They
contain no recorded person, voice, camera footage, account data, or device
identity.

| File | Content | SHA-256 |
| --- | --- | --- |
| `audio_g711a_8khz_mono_20ms_10s_500packets.g711a` | 10-second 440 Hz sine tone, G711A, 8 kHz, mono, 20 ms packets | `c74cc3acdca5231d34378e2341410f409a4b4e1f14992000e25e09c9c14b57b2` |
| `video_h264_annexb_640x480_15fps_10s_150frames.h264` | 10-second synthetic test pattern, H264 Annex-B, 640x480, 15 fps, encoded with x264 | `0d53af92dbdfa961b8d3f2320f5655a307f5837625f25eb826442c1aee1ce25c` |

The generated fixtures are distributed under the repository root license.
Codec implementations and TiRTC remain subject to their own licenses.
Replacing either file changes `storage.bin`; rebuild the project and repeat the
relevant media and hardware checks after any replacement.
