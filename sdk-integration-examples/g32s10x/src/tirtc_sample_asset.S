    .section .rodata.tirtc_sample_media, "a", @progbits
    .balign 4
    .global g_tirtc_sample_asset_start
    .global g_tirtc_sample_asset_end

g_tirtc_sample_asset_start:
    .incbin "application/tirtc_g32s10x_wifi_link_demo/assets/sample_demo_mjpeg_pcma.avi"
g_tirtc_sample_asset_end:
    .balign 4

    .section .note.GNU-stack, "", @progbits
