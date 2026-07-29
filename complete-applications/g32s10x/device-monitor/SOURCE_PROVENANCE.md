# 来源与验证边界

## 来源快照

| 项目 | 值 |
| --- | --- |
| 源工程 | `g32s10x_tirtc` |
| 源分支 | `main` |
| 源标签 | `v0.1.1` |
| 源提交 | `563015292c0edde3be977f244bfe46bd03fdc300` |
| 源工作树 | 标签指向上述提交；同步时源工作树干净 |
| APP 版本 | `0.1.1` |
| TiRTC SDK 版本 | `2.2.1` |
| SDK 交付状态 | `candidate` |
| 候选 `libTiRTC.a` SHA-256 | `33e889d70d4459587faf57f611e9648d52accb2bc02d0ec75205a52e9ca27fba` |

本目录按 `v0.1.1` 的不可变源码快照整理。APP 版本、SDK 版本和来源提交分别记录，
不能用最小示例的标签或构建证据替代完整应用的发布证据。

## 公开筛选

已纳入：

- G32S10X 应用业务、UI、媒体与云端接入源码。
- TiRTC SDK 公共头文件、G32 静态库及其静态清单。
- 君正 package/Kconfig 集成、发布 defconfig 和必要的平台覆盖文件。
- 不含本机默认路径的源码检查、覆盖层安装和构建入口。

已排除：

- 展开的君正 SDK、RISC-V 工具链和 Cloner 安装目录。
- `building/`、固件、ELF、YAFFS、日志和本机调试缓存。
- WiFi 密码、设备 Secret、访问密钥和一次性 Token。
- 个人工作区路径与内部发布编排资料。

## 静态清单

`sdk/manifest/` 保存 SDK 来源提交、工具链契约、静态库大小与哈希、下层库嵌入关系
以及符号扫描结果。`symbols-undefined-by-object.txt` 中的构建机前缀已统一替换为
`<TIRTC_BUILD_ROOT>`；该字段只用于展示对象来源，未修改 `libTiRTC.a` 或清单中的
库哈希。

开发者可使用以下命令核对静态库：

```bash
sha256sum sdk/lib/g32/libTiRTC.a
```

期望 SHA-256：

```text
33e889d70d4459587faf57f611e9648d52accb2bc02d0ec75205a52e9ca27fba
```

## 验证边界

本次候选整理执行源码结构、个人路径、凭据模式、Markdown 链接、版本与静态库哈希
检查。固件构建、Cloner 写入和目标板功能验证使用各自独立证据。
