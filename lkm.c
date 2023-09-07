/***************************************************************************************
 * Copyright 2023 Infineon Technologies AG ( www.infineon.com ).                       *
 * All rights reserved.                                                                *
 *                                                                                     *
 * Licensed  Material-Property of Infineon Technologies AG.                            *
 * This software is made available solely pursuant to the terms of Infineon            *
 * Technologies AG agreement which governs its use. This code and the information      *
 * contained in it are proprietary and confidential to Infineon Technologies AG.       *
 * No person is allowed to copy, reprint, reproduce or publish any part of this code,  *
 * nor disclose its contents to others, nor make any use of it, nor allow or assist    *
 * others to make any use of it - unless by prior Written express authorization of     *
 * Infineon Technologies AG and then only to the extent authorized.                    *
 *                                                                                     *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,            *
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY,           *
 * FITNESS FOR A PARTICULAR PURPOSE, OR NON-INFRINGEMENT, ARE DISCLAIMED.  IN NO       *
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,     *
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,                 *
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;         *
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY             *
 * WHETHER IN  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR            *
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF              *
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.                                          *
 *                                                                                     *
 ***************************************************************************************/
/**
 * @file   lkm.c
 * @date   July, 2023
 * @brief  ...
 */
#define pr_fmt(fmt) "%s:%s:%s:%d: " fmt, KBUILD_MODNAME, __FILE__, __func__, __LINE__

#include <linux/init.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include "lkm.h"

struct this_context {
    struct common_context common;

    chardev_context *cd_ctx;
    resource_manager_context *rm_ctx;
    realtime_context *rt_ctx;

    const struct attribute_group *attr_groups[3]; /* Based of number of attribute group + 1 for NULL termination */
};

struct session_context {
    struct common_context common;

    struct this_context *parent;
    size_t cmd_len;
    size_t resp_len;
    char cmd[256];
    char resp[256];
};

static struct this_context *this;

static struct this_context *lkm_context_cast(lkm_context *ctx)
{
    if (ctx != NULL && ctx->magic == LKM_CONTEXT_MAGIC) {
        return container_of(ctx, struct this_context, common);
    }

    return NULL;
}

static struct session_context *lkm_session_context_cast(lkm_session_context *ctx)
{
    if (ctx != NULL && ctx->magic == LKM_SESSION_CONTEXT_MAGIC) {
        return container_of(ctx, struct session_context, common);
    }

    return NULL;
}

static int lkm_open(realtime_child_context *child_ctx,
                    realtime_child_session_context **child_s_ctx)
{
    int rc;
    struct this_context *this = lkm_context_cast(child_ctx);
    struct session_context *session_ctx;

    session_ctx = kzalloc(sizeof(*session_ctx), GFP_KERNEL);
    if (!session_ctx) {
        rc = -ENOMEM;
        goto out;
    }
    session_ctx->common.magic = LKM_SESSION_CONTEXT_MAGIC;

    /* Sanity check */
    if (lkm_session_context_cast(&session_ctx->common) != session_ctx) {
        rc = -EFAULT;
        goto out_free_session_ctx;
    }

    *child_s_ctx = &session_ctx->common;

    session_ctx->parent = this;

    return 0;

out_free_session_ctx:
    kfree(session_ctx);
out:
    return rc;
}

static ssize_t lkm_write1(realtime_child_session_context *child_s_ctx,
                          char **buf)
{
    struct session_context *session_ctx = lkm_session_context_cast(child_s_ctx);

    *buf = session_ctx->cmd;

    return sizeof(session_ctx->cmd);
}

static ssize_t lkm_write2(realtime_child_session_context *child_s_ctx,
                          size_t len)
{
    struct session_context *session_ctx = lkm_session_context_cast(child_s_ctx);

    if (len > sizeof(session_ctx->cmd)) {
        return -EINVAL;
    }

    session_ctx->cmd_len = len;
    return len;
}

static int lkm_execute(realtime_child_session_context *child_s_ctx)
{
    struct session_context *session_ctx = lkm_session_context_cast(child_s_ctx);

    switch (session_ctx->cmd[0]) {
        case 0xBB:
            do {
                int delay_ms;

                if (session_ctx->cmd_len > 1) {
                    delay_ms = session_ctx->cmd[1];
                } else {
                    delay_ms = 500;
                }

                while (delay_ms--) {
                    udelay(1000);
                }
            } while (false);

            session_ctx->resp[0] = 0x90;
            session_ctx->resp[1] = 0x00;
            session_ctx->resp_len = 2;
            break;
        default:
            session_ctx->resp[0] = 0x6F;
            session_ctx->resp[1] = 0x00;
            session_ctx->resp_len = 2;
            break;
    }

    session_ctx->cmd_len = 0;
    memset(session_ctx->cmd, 0, sizeof(session_ctx->cmd));

    return 0;
}

static ssize_t lkm_read1(realtime_child_session_context *child_s_ctx,
                         char **buf)
{
    struct session_context *session_ctx = lkm_session_context_cast(child_s_ctx);

    *buf = session_ctx->resp;

    return session_ctx->resp_len;
}

static ssize_t lkm_read2(realtime_child_session_context *child_s_ctx,
                         size_t len)
{
    struct session_context *session_ctx = lkm_session_context_cast(child_s_ctx);

    if (unlikely(session_ctx->resp_len == 0)) {
        return -EINVAL;
    }

    if (!len) {
        /* End of reading */
        session_ctx->resp_len = 0;
        memset(session_ctx->resp, 0, sizeof(session_ctx->resp));
    }

    return len;
}

static int lkm_release(realtime_child_session_context *child_s_ctx)
{
    struct session_context *session_ctx = lkm_session_context_cast(child_s_ctx);
    kfree(session_ctx);
    return 0;
}

static const struct realtime_child_funcs rt_child = {
    .open = lkm_open,
    .write1 = lkm_write1,
    .write2 = lkm_write2,
    .execute = lkm_execute,
    .read1 = lkm_read1,
    .read2 = lkm_read2,
    .release = lkm_release,
};

static int __init mod_init(void)
{
    int rc;
    const struct resource_manager_child_funcs *rm_child;
    const struct chardev_child_funcs *cd_child;
    const struct attribute_group *rt_attr_group;
    const struct attribute_group *rm_attr_group;

    pr_info("mod_init started.\n");

    /* Bring-up the lkm layer. */

    this = kzalloc(sizeof(*this), GFP_KERNEL);
    if (!this) {
        rc = -ENOMEM;
        goto out;
    }
    this->common.magic = LKM_CONTEXT_MAGIC;

    /* Sanity check */
    if (lkm_context_cast(&this->common) != this) {
        rc = -EFAULT;
        goto out_free_this;
    }

    /* Bring-up the realtime layer and hook lkm layer to it. */

    rc = realtime_init(&this->rt_ctx);
    if (rc) {
        goto out_free_this;
    }

    rc = realtime_set_hook(this->rt_ctx,
                           (realtime_child_context *)this,
                           &rt_child);
    if (rc) {
        goto out_free_this;
    }

    rm_child = realtime_get_hook_resource_manager();
    if (!rm_child) {
        rc = -EFAULT;
        goto out_free_this;
    }

    rt_attr_group = realtime_get_attribute_group(this->rt_ctx);

    /* Bring-up the resource-manager layer and hook realtime layer to it. */

    rc = resource_manager_init(&this->rm_ctx);
    if (rc) {
        goto out_free_this;
    }

    rc = resource_manager_set_hook(this->rm_ctx,
                                   (resource_manager_child_context *)this->rt_ctx,
                                   rm_child);
    if (rc) {
        goto out_free_this;
    }

    cd_child = resource_manager_get_hook_chardev();
    if (!cd_child) {
        rc = -EFAULT;
        goto out_free_this;
    }

    rm_attr_group = resource_manager_get_attribute_group(this->rm_ctx);

    /* Bring-up the chardev layer and hook realtime layer to it. */

    this->attr_groups[0] = rt_attr_group;
    this->attr_groups[1] = rm_attr_group;
    this->attr_groups[2] = NULL;

    rc = chardev_init(&this->cd_ctx, this->attr_groups);
    if (rc) {
        goto out_free_this;
    }

    rc = chardev_set_hook(this->cd_ctx,
                          (chardev_child_context *)this->rm_ctx,
                          cd_child);
    if (rc) {
        goto out_free_this;
    }

    rc = 0;
    goto out;

out_free_this:
    kfree(this);
out:
    pr_info("mod_init ended.\n");
    return rc;
}

static void __exit mod_exit(void)
{
    pr_info("mod_exit started.\n");

    chardev_exit(&this->cd_ctx);
    resource_manager_exit(&this->rm_ctx);
    realtime_exit(&this->rt_ctx);
    kfree(this);

    pr_info("mod_exit ended.\n");
}

module_init(mod_init);
module_exit(mod_exit);

MODULE_DESCRIPTION("A Linux Kernel Module Reference");
MODULE_LICENSE("GPL");
MODULE_VERSION("202306");
