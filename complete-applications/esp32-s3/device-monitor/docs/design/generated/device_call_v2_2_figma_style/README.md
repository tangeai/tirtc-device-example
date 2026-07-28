# Device Call UI v2.2 - Figma style

This is the current visual review set for the 320 x 240 Device Call flow.

## Source of truth

Figma file: `qc7uF59aNxFxfb7siqod7h`, page `12:2`.

The following real Figma nodes were read with design context and screenshot
tools before this set was rendered:

| Node | Figma screen |
| --- | --- |
| `233:2` | Call home |
| `233:121` | Add contact |
| `233:146` | Scan information modal |
| `233:181` | Contact list |
| `233:215` | Incoming call modal |
| `233:343` | In-call controls |
| `129:214` | TiRTC field editor and keyboard |
| `129:311` | TiRTC scan-fill screen |

Downloaded source screenshots are stored in
`../../figma_reference/device_call/`.

## Locked visual rules

- Physical and logical size: `320 x 240`.
- Header: `28 px`, background `#f7fbfe`, border `#d6e4ef`.
- Page background: `#e8f3fa`.
- Primary text: `#10233b`, Noto Sans SC Black for titles.
- Main action: `#21c783`; signal state: `#23c987`.
- Secondary action: `#e9f5ff` with `#2f82d7` text.
- Destructive action: `#ffe7e7`, `#f15a5a`, `#e44747`.
- Cards and fields: white, 1 px blue-gray border, 7-8 px radius.
- Content inset: `8 px`; normal page controls are 34, 38, or 44 px high.
- Compact information density is intentional. Do not enlarge this into a
  phone-style UI.

## Screens

1. `01-call-home.png`
2. `02-add-contact.png`
3. `03-camera-scan.png`
4. `04-scan-result.png`
5. `05-device-id-keyboard.png`
6. `06-my-device-code.png`
7. `07-contact-list.png`
8. `08-contact-empty.png`
9. `09-outgoing-ringing.png`
10. `10-caller-connecting.png`
11. `11-incoming-call.png`
12. `12-in-call.png`
13. `13-call-waiting.png`
14. `14-switch-confirm.png`
15. `15-result-banner.png`

`00-overview.png` is the review sheet. The editable deterministic prototype is
`../../prototype/device_call_figma_style/index.html`.

## Verification

- Rendered with local Chrome through Playwright.
- All 15 outputs are exactly `320 x 240`.
- DOM bounds audit found no element outside a screen.
- Text clipping audit found no clipped labels.
- Scanner background represents the runtime camera feed; only its overlay is a
  fixed UI element.

The previous `device_call_v2_1` image-generation set is rejected and must not
be used as a visual reference.
