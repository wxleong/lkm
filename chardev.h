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
 * @file   chardev.h
 * @date   July, 2023
 * @brief  Manage the initialization and operation of character devices.
 */
#ifndef __CHARDEV_H__
#define __CHARDEV_H__

#include <linux/cdev.h>
#include "common.h"

#define CHARDEV_CONTEXT_MAGIC      0x3D59F79E

typedef struct common_context chardev_context;

typedef struct common_context chardev_child_context;
typedef struct common_context chardev_child_session_context;

typedef int
(*CHARDEV_CHILD_OPEN_FUNC)(chardev_child_context *child_ctx,
                           chardev_child_session_context **child_s_ctx);
/**
 * typedef CHARDEV_CHILD_READ1_FUNC - To retrieve a buffer for reading.
 * @child_s_ctx: chardev's child session context.
 * @buf: A buffer pointer will be returned here for reading.
 *
 * Returns the size of data that can be read from @buf. In case of an error,
 * a negative value is returned.
 */
typedef ssize_t
(*CHARDEV_CHILD_READ1_FUNC)(chardev_child_session_context *child_s_ctx,
                            char **buf);
/**
 * typedef CHARDEV_CHILD_READ2_FUNC - To notify the progress of reading.
 * @child_s_ctx: chardev's child session context.
 * @len: The number of bytes that have been read by caller.
 *
 * If @len is zero, it indicates that the read operation
 * has been completed, regardless of whether there are still
 * remaining bytes to be read.
 *
 * Returns:
 * The returned value should be equal to @len. In case of an
 * error, a negative value is returned.
 */
typedef ssize_t
(*CHARDEV_CHILD_READ2_FUNC)(chardev_child_session_context *child_s_ctx,
                            size_t len);
/**
 * typedef CHARDEV_CHILD_WRITE1_FUNC - To retrieve a buffer for writing.
 * @child_s_ctx: chardev's child session context.
 * @buf: A buffer pointer will be returned here for writing.
 *
 * The @buf can be written to freely prior to calling _WRITE2_FUNC
 *
 * Returns the size of @buf. In case of an error, a negative
 * value is returned.
 */
typedef ssize_t
(*CHARDEV_CHILD_WRITE1_FUNC)(chardev_child_session_context *child_s_ctx,
                             char **buf);
/**
 * typedef CHARDEV_CHILD_WRITE2_FUNC - To update the progress of writing.
 * @child_s_ctx: chardev's child session context.
 * @len: The number of bytes that have been written by caller.
 *
 * Partial writing is not supported. Invoking this function with
 * a non-zero @len will initiate the processing of received
 * data. If the @len value is zero, the function will exit
 * without performing any action, returning zero.
 *
 * Returns:
 * The returned value should be equal to @len. In case of an
 * error, a negative value is returned.
 */
typedef ssize_t
(*CHARDEV_CHILD_WRITE2_FUNC)(chardev_child_session_context *child_s_ctx,
                             size_t len);
typedef __poll_t
(*CHARDEV_CHILD_POLL_FUNC)(chardev_child_session_context *child_s_ctx,
                           wait_queue_head_t **waitq);
typedef int
(*CHARDEV_CHILD_RELEASE_FUNC)(chardev_child_session_context *child_s_ctx);

struct chardev_child_funcs {
    CHARDEV_CHILD_OPEN_FUNC         open;

    CHARDEV_CHILD_WRITE1_FUNC       write1; /* To retrieve write buffer */
    CHARDEV_CHILD_WRITE2_FUNC       write2; /* To notify the progress of write */

    CHARDEV_CHILD_READ1_FUNC        read1; /* To retrieve read buffer */
    CHARDEV_CHILD_READ2_FUNC        read2; /* To notify the progress of read */

    CHARDEV_CHILD_POLL_FUNC         poll;
    CHARDEV_CHILD_RELEASE_FUNC      release;
};

/**
 * chardev_init - Initialize chardev context.
 * @cd_ctx: Returns the chardev context.
 * @attr_groups: Input for attribute groups.
 *
 * The array of attribute groups is terminated with a NULL entry.
 *
 * Returns:
 * On success, 0 is returned. In case of an error,
 * a negative value is returned.
 */
int chardev_init(chardev_context **cd_ctx,
                 const struct attribute_group **attr_groups);
void chardev_exit(chardev_context **);
int chardev_set_hook(chardev_context *,
                     chardev_child_context *,
                     const struct chardev_child_funcs *);
#endif
