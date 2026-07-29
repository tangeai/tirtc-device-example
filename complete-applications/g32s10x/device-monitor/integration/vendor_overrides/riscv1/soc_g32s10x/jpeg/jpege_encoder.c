#include <common.h>
#include <errno.h>
#include <driver/cache.h>
#include <driver/jpeg.h>

#include "jpege_drv.h"
#include "jpege_ops.h"

struct jpege_encoder {
    void *src_buf_vaddr;
    void *dst_buf_vaddr;
    unsigned int src_size;
    unsigned int dst_size;

    struct jpege_info start;

    struct jpege_encoder_ctx ctx;
};

struct jpege_encoder *soc_jpege_encoder_init(struct jpege_encoder_param *param)
{
    jpege_init();

    struct jpege_encoder *encoder = (struct jpege_encoder *)malloc(sizeof(struct jpege_encoder));
    if (!encoder) {
        jz_log_error(JZ_JPEG_MOD, "error: malloc failed\n");
        goto deinit_encoder;
    }
    memset(encoder, 0, sizeof(struct jpege_encoder));

    encoder->ctx.width = param->width;
    encoder->ctx.height = param->height;
    encoder->ctx.in_fmt = param->in_fmt;
    encoder->ctx.mode = param->mode;
    encoder->ctx.reset = param->reset;
    encoder->ctx.quality = param->quality;
    encoder->ctx.linkage = param->linkage;

    jpege_encoder_set_subsample(&encoder->ctx);
    return encoder;

deinit_encoder:
    free(encoder);
    jpege_deinit();
    return NULL;
}

void soc_jpege_encoder_deinit(struct jpege_encoder *encoder)
{
    if (!encoder)
        return;

    free(encoder->src_buf_vaddr);
    free(encoder->dst_buf_vaddr);

    jpege_deinit();

    free(encoder);
}

static int check_alloc_src_mem(struct jpege_encoder *encoder, int size)
{
    unsigned int required_size;

    if (size <= 0)
        return -EINVAL;

    required_size = (unsigned int)size;
    if (encoder->src_buf_vaddr &&
        encoder->src_size >= required_size)
        return EOK;

    if (encoder->src_buf_vaddr) {
        free(encoder->src_buf_vaddr);
        encoder->src_buf_vaddr = NULL;
        encoder->src_size = 0;
    }

    if (!(encoder->src_buf_vaddr = memalign(256, required_size))) {
        jz_log_error(JZ_JPEG_MOD, "error: memalign failed\n");
        return -ENOMEM;
    }
    encoder->src_size = required_size;

    return EOK;
}

static int check_alloc_dst_mem(struct jpege_encoder *encoder, int size)
{
    unsigned int required_size;

    if (size <= 0)
        return -EINVAL;

    required_size = (unsigned int)size;
    if (encoder->dst_buf_vaddr &&
        encoder->dst_size >= required_size)
        return EOK;

    if (encoder->dst_buf_vaddr) {
        free(encoder->dst_buf_vaddr);
        encoder->dst_buf_vaddr = NULL;
        encoder->dst_size = 0;
    }

    if (!(encoder->dst_buf_vaddr = memalign(256, required_size))) {
        jz_log_error(JZ_JPEG_MOD, "error: memalign failed\n");
        return -ENOMEM;
    }
    encoder->dst_size = required_size;

    return EOK;
}

void soc_jpege_encoder_free_output_buf(struct jpege_encoder_output *out)
{
    if (!out)
        return;

    if (out->data)
        free(out->data);

    free(out);
}

struct jpege_encoder_output *soc_jpege_encoder_alloc_output_buf(struct jpege_encoder *encoder)
{
    size_t size = encoder->ctx.width * encoder->ctx.height * 2U + 4096U;
    struct jpege_encoder_output *out = (struct jpege_encoder_output *)malloc(sizeof(struct jpege_encoder_output));
    if (!out) {
        jz_log_error(JZ_JPEG_MOD, "error: malloc failed\n");
        return NULL;
    }
    memset(out, 0, sizeof(*out));

    out->data_size = size;

    if (!(out->data = memalign(256, out->data_size))) {
        jz_log_error(JZ_JPEG_MOD, "error: memalign failed\n");
        goto free_en_out;
    }

    return out;
free_en_out:
    soc_jpege_encoder_free_output_buf(out);
    return NULL;
}

int soc_jpege_encoder_encode(struct jpege_encoder *encoder, void *src, int src_size, struct jpege_encoder_output *out)
{
    struct jpege_padding_info *pinfo;
    void *dst;
    int input_size;
    int dst_size;
    int err;
    int result = -EINVAL;

    if (!encoder || !src || src_size <= 0 || !out || !out->data || out->data_size <= 0)
        return -EINVAL;

    pinfo = get_pinfo(&encoder->ctx);
    if (!pinfo)
        return -ENOMEM;

    input_size = jpege_get_input_size(encoder->ctx.width, encoder->ctx.height, encoder->ctx.in_fmt);
    if (input_size <= 0 || src_size < input_size)
        goto out;

    dst = out->data;
    dst_size = out->data_size;

    switch (encoder->ctx.in_fmt) {
    case JPEGE_PIX_FMT_YUYV:
        if ((unsigned long)src % 256) {
            if (check_alloc_src_mem(encoder, input_size) != EOK) {
                result = -ENOMEM;
                goto out;
            }

            memcpy(encoder->src_buf_vaddr, src, input_size);
            flush_dcache_force((unsigned long)encoder->src_buf_vaddr, input_size);
            encoder->start.in.src_addr = encoder->src_buf_vaddr;
        } else {
            flush_dcache_force((unsigned long)src, src_size);
            encoder->start.in.src_addr = src;
        }
        break;
    case JPEGE_PIX_FMT_Y:
    case JPEGE_PIX_FMT_NV12:
        if (pinfo->fill_num_per_line) {
            int width_align = __align_up(encoder->ctx.width, 16);
            int fill_size = jpege_get_input_size(width_align, encoder->ctx.height, encoder->ctx.in_fmt);
            if (check_alloc_src_mem(encoder, fill_size) != EOK) {
                result = -ENOMEM;
                goto out;
            }

            fillcpy(encoder->src_buf_vaddr, src, pinfo);
            flush_dcache_force((unsigned long)encoder->src_buf_vaddr, fill_size);
            encoder->start.in.src_addr = encoder->src_buf_vaddr;
        } else if ((unsigned long)src % 256) {
            if (check_alloc_src_mem(encoder, input_size) != EOK) {
                result = -ENOMEM;
                goto out;
            }

            memcpy(encoder->src_buf_vaddr, src, input_size);
            flush_dcache_force((unsigned long)encoder->src_buf_vaddr, input_size);
            encoder->start.in.src_addr = encoder->src_buf_vaddr;
        } else {
            flush_dcache_force((unsigned long)src, src_size);
            encoder->start.in.src_addr = src;
        }
        break;
    default:
        jz_log_error(JZ_JPEG_MOD, "Unsupported format %d\n", encoder->ctx.in_fmt);
        goto out;
    }

    if ((unsigned long)dst % 256) {
        if (check_alloc_dst_mem(encoder, dst_size) != EOK) {
            result = -ENOMEM;
            goto out;
        }

        invalidate_dcache_force((unsigned long)encoder->dst_buf_vaddr, encoder->dst_size);
        encoder->start.out.dst_addr = encoder->dst_buf_vaddr;
    } else {
        invalidate_dcache_force((unsigned long)dst, dst_size);
        encoder->start.out.dst_addr = dst;
    }

    encoder->start.in.frame_width = encoder->ctx.width;
    encoder->start.in.frame_height = encoder->ctx.height;
    encoder->start.in.pix_fmt = encoder->ctx.in_fmt;
    encoder->start.in.mode = encoder->ctx.mode;
    encoder->start.in.reset = encoder->ctx.reset;
    encoder->start.in.qp = encoder->ctx.quality;
    encoder->start.in.scaler = encoder->ctx.linkage;

    err = jpege_start(&encoder->start);
    if (err < 0) {
        jz_log_error(JZ_JPEG_MOD, "error: jpege_start failed %d\n", err);
        result = -EPERM;
        goto out;
    }

    encoder->start.out.imagesize = jpege_get_output_size();
    if (encoder->start.out.imagesize > (unsigned int)dst_size) {
        jz_log_error(JZ_JPEG_MOD, "error: output buffer too small (%u > %d)\n",
                     encoder->start.out.imagesize, dst_size);
        result = -ENOSPC;
        goto out;
    }

    if ((unsigned long)dst % 256) {
        invalidate_dcache_force((unsigned long)encoder->dst_buf_vaddr, encoder->dst_size);
    } else {
        invalidate_dcache_force((unsigned long)dst, dst_size);
    }

    out->width = encoder->ctx.width;
    out->height = encoder->ctx.height;
    out->data_size = encoder->start.out.imagesize;

    if (encoder->dst_buf_vaddr && encoder->dst_buf_vaddr != dst) {
        memcpy(dst, encoder->dst_buf_vaddr, out->data_size);
        flush_dcache_force((unsigned long)dst, out->data_size);
    }

    result = EOK;
out:
    free(pinfo);
    return result;
}
