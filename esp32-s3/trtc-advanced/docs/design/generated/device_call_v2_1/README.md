# Device Call UI v2.1 image set

> Status: rejected visual draft. This set did not use the real Figma node
> metrics and must not be used for firmware implementation. The replacement is
> `../device_call_v2_2_figma_style/`.

Source of truth: `../../DEVICE_CALL_BUSINESS_AND_WIREFRAME_V2_CN.md`.

The root PNG files are the high-resolution image-generation outputs. The
`320x240/` directory contains mechanically resized previews at the device's
physical screen resolution.

`00-overview.png` is a review-only contact sheet assembled from all fifteen
physical-resolution previews. It is not a production screen.

| File | Screen/state |
| --- | --- |
| `01-p1-contacts-loaded.png` | P1 contacts, loaded |
| `02-p1-contacts-empty.png` | P1 contacts, empty |
| `03-p2-add-methods.png` | P2 add-device methods |
| `04-t2-camera-scanner.png` | T2 full-screen camera scanner |
| `05-o4-scan-result.png` | O4 scanned-device confirmation |
| `06-p2m-manual-add.png` | P2M manual add |
| `07-t1-device-id-keyboard.png` | T1 Device ID keyboard |
| `08-t3-my-device-qr.png` | T3 local device QR |
| `09-p3-outgoing-ringing.png` | P3 outgoing ringing |
| `10-p3-caller-connecting.png` | P3 caller connecting |
| `11-p4-incoming.png` | P4 idle incoming call |
| `12-p5-in-call.png` | P5 established call |
| `13-o2-call-waiting.png` | O2 incoming call while busy |
| `14-o3-switch-confirm.png` | O3 high-risk call-switch confirmation |
| `15-o1-result-banner.png` | O1 transient result banner |

## Shared generation prompt

- One screen per image, 4:3 landscape, representing a 320 x 240 logical UI.
- Pale icy-blue background, white surfaces, thin blue-gray borders, dark navy
  text, mint-green primary actions, coral-red destructive actions.
- Compact layout, corner radius no greater than 8 px, minimum 40 x 40 logical
  touch targets, no shadows or gradients.
- Device Call only. Do not include WeChat VoIP, video calling, peer secrets,
  pair keys, connection tokens, marketing copy, or explanatory paragraphs.
- QR output contains only `{"device_id":"..."}`; scanning is the primary add
  path and manual Device ID input is the fallback.

These images are visual review artifacts, not production UI assets. Production
coordinates and behavior continue to follow the source-of-truth document.
