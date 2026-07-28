# 设备呼叫 v2.2 Figma 交付说明

## 设计入口

- Figma 页面：`设备呼叫 v2.2 / Auto Layout`（`365:2`）
- 页面根容器：`Device Call v2.2 / Screen Set`（`365:3`）
- 画布规格：`320 x 240`
- 完整预览：`figma_reference/device_call/figma-v2-2-full.png`

Figma 链接：

<https://www.figma.com/design/qc7uF59aNxFxfb7siqod7h/S3-%E7%9B%91%E6%8E%A7%E7%AB%AF-App-%E7%95%8C%E9%9D%A2?node-id=365-2>

## 页面结构

设计文件不是按坐标堆叠。普通页面统一采用以下 Auto Layout 层级：

```text
Screen (320 x 240, Vertical)
├── Header (320 x 28, Horizontal)
└── Content (320 x 212, Vertical/Horizontal)
    ├── Section
    └── Reusable Component Instance
```

只有确实需要叠放的状态使用 `layoutMode=NONE`：

- 全屏摄像头预览与取景框
- 扫码结果弹层
- 空闲来电弹层
- 通话中来电弹层
- 切换通话确认弹层

这些节点都显式命名为 `Overlay Stack`，其底层页面仍保持 Auto Layout。

## C 代码分层映射

```text
Application / 业务与资源
  main/application/app_call.c
  main/application/app_call_contacts.c
        ↓
UI State / 页面状态
  display_status_t
  display_actions_t
        ↓
Screen / 页面布局
  display_build_call_page()
  display_build_call_add_page()
  display_build_call_add_edit_page()
  display_build_call_scan_page()
  display_build_call_list_page()
  display_build_call_active_page()
        ↓
Component / 可复用控件
  display_create_call_menu_button()
  display_create_call_qr()
  display_create_call_add_field_row()
  display_create_call_contact_row()
  display_create_call_duration_row()
  display_create_call_volume_row()
        ↓
Driver / 屏幕与摄像头
  display driver
  camera driver
```

Figma 的屏幕标签已经标注对应的 C builder、`display_status_t` 状态字段或
`display_actions_t` 动作契约。后续实现应保持该所有权关系，不要让业务资源释放、RTC
生命周期或联系人持久化散落到 UI 点击回调中。

## 页面清单

- Row 1（`372:2`）：呼叫首页、添加联系人、全屏扫码、扫码结果
- Row 2（`373:136`）：Device ID 输入、本机设备码、联系人列表、联系人空状态
- Row 3（`378:184`）：呼叫中、建立连接、空闲来电、通话中
- Row 4（`381:364`）：通话中来电、切换确认、通话结束结果

共 15 个页面/状态，全部通过 `320 x 240` 边界检查。

## 复用资产

- 语义变量集合：`S3 Device UI v2.2`（`VariableCollectionId:358:2`）
- 组件区：`S3 Device Call Components v2.2`（`359:2`）
- 状态组件：`371:2` - `371:7`
- 联系人、音量、时长、结果组件：`361:*`
- 操作按钮组件：`360:*`
- 本机二维码组件：`366:106`

实现时优先复用这些组件和语义变量，不要在每个页面重新创建相同颜色、间距和控件。
