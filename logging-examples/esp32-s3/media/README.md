# 示例测试输入

本目录包含日志示例使用的两项合成媒体测试输入，以及五项 AI 指令提示音。
它们只用于演示协议、媒体收发计数和 AT 流程，不代表真实摄像头、麦克风、
扬声器或产品媒体质量。

| 文件 | 内容与来源 | SHA-256 |
| --- | --- | --- |
| `audio_g711a_8khz_mono_20ms_2s_100packets.g711a` | 合成 440 Hz 正弦音；2 秒，G711A，8 kHz，单声道，20 ms/包 | `60fa7335ca0aebddc119476c0b46b3faf047affd5a7c5cb24b0199fdf845956f` |
| `video_h264_annexb_640x480_15fps_8s_120frames.h264` | 合成测试图；8 秒，H264 Annex-B，640x480，15 fps | `27735e98fcab14ddd916d6dd0deb86a4fa5d2b04472605234115a681db67b8d8` |
| `ai_story.g711a` | Microsoft Huihui Desktop 本地合成：“讲个百字内的故事。” | `67381051a72ba8e29822d1f2267a7c5115fa849948535939a59d4fae479dbffd` |
| `ai_joke.g711a` | Microsoft Huihui Desktop 本地合成：“讲个简短笑话。” | `a7bb51c454b5e4c1251502bf8d3ab6d974b8139205f4130599053df980bec888` |
| `ai_weather.g711a` | Microsoft Huihui Desktop 本地合成：“今天天气怎么样？” | `8d080adb9b1b83dd02fab2c8f41ee8534aacb13225cffc71a966b07aa467df63` |
| `ai_call_xiaozhang.g711a` | Microsoft Huihui Desktop 本地合成：“呼叫小张。” | `530c6041bd4b51c24176ffa1c0434954687f2f18bb0810a496d05ba18859173c` |
| `ai_call_xiaoli.g711a` | Microsoft Huihui Desktop 本地合成：“呼叫小李。” | `730c29cd4dbf4504a2ee8bdb8742e6a1d296794811115235727ed6680a35a480` |

五项提示音可用 `tools/generate_ai_prompts.ps1` 在安装了相同语音引擎的本机
重新生成。所有 G711A 文件均按 160 字节包对齐；替换任何媒体后都需重建
`storage.bin`，并重新执行相关媒体和硬件检查。

这里记录的是素材来源和技术用途，不额外声明 Microsoft Huihui Desktop、
编解码器或 TiRTC 相关内容的授权范围；使用和再分发时应分别遵守其适用
条款。
