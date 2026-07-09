# Local Patch Maintenance

The local patch is stored in git at:

    /home/wty/work/tirtc-nano-release-master
    branch: local/esp32-s3-p4-delivery-patches
    commit: 6c8f02580761e6f3991975e36b14aaf8b808bf05

Recommended update flow:

1. Fetch upstream master.
2. Rebase local/esp32-s3-p4-delivery-patches on origin/master.
3. Resolve only these two expected areas if upstream changes them:
   - platform/plat_freertos.c
   - esp32p4.mak
4. Rebuild S3/P4 with p2p=kcp nossl=y.
5. Package from the rebased local branch.

Do not push this localfix branch unless we decide to submit these fixes upstream again.
