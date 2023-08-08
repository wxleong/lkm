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
 * @file   realtime.h
 * @date   July, 2023
 * @brief  Header file
 */
#ifndef __REALTIME_H__
#define __REALTIME_H__

#include "resource-manager.h"

#define REALTIME_CONTEXT_MAGIC            0x32090AA0
#define REALTIME_SESSION_CONTEXT_MAGIC       0x4EA29522

#define REALTIME_CMD_BUFFER_SIZE  8
#define REALTIME_RESP_BUFFER_SIZE 2048

typedef struct common_context realtime_context;
typedef struct common_context realtime_session_context;

typedef struct common_context realtime_child_context;
typedef struct common_context realtime_child_session_context;

typedef int
(*REALTIME_CHILD_OPEN_FUNC)(realtime_child_context *,
                            realtime_child_session_context **);
/**
 * typedef REALTIME_CHILD_WRITE1_FUNC - Retrieve the write buffer.
 * @child_s_ctx: Session context.
 * @buf: A buffer pointer will be returned here for writing.
 *
 * The @buf can be written to freely prior to calling _WRITE2_FUNC.
 *
 * Returns the size of @buf. In case of an error, a negative
 * value is returned.
 */
typedef ssize_t
(*REALTIME_CHILD_WRITE1_FUNC)(realtime_child_session_context *child_s_ctx,
                              char **buf);
/**
 * typedef REALTIME_CHILD_WRITE2_FUNC - Set the size of the data written into the write buffer.
 * @child_s_ctx: Session context.
 * @len: The number of bytes that have been written by caller.
 *
 * Returns:
 * The returned value should be equal to @len. 0 indicates no action
 * is performed. In case of an error, a negative value is returned.
 */
typedef ssize_t
(*REALTIME_CHILD_WRITE2_FUNC)(realtime_child_session_context *child_s_ctx,
                              size_t len);
/**
 * typedef REALTIME_CHILD_EXECUTE_FUNC - The data will be processed here.
 * @child_s_ctx: Session context.
 *
 * The received data will be processed in this function, and the corresponding
 * response can be retrieved through the _READ1_FUNC.
 *
 * Returns:
 * On success, 0 is returned. In case of an error, a negative value is returned.
 */
typedef int
(*REALTIME_CHILD_EXECUTE_FUNC)(realtime_child_session_context *child_s_ctx);
/**
 * typedef REALTIME_CHILD_READ1_FUNC - Retrieve the read buffer.
 * @child_s_ctx: Session context.
 * @buf: A buffer pointer will be returned here for reading.
 *
 * Returns the size of the response that can be read from @buf. In case of an error,
 * a negative value is returned.
 */
typedef ssize_t
(*REALTIME_CHILD_READ1_FUNC)(realtime_child_session_context *child_s_ctx,
                             char **buf);
/**
 * typedef REALTIME_CHILD_READ2_FUNC - Update the progress of a reading operation.
 * @child_s_ctx: Session context.
 * @len: The number of bytes that have been read by caller.
 *
 * If @len is zero, it indicates that the read operation
 * has been completed, regardless of whether there are still
 * remaining bytes to be read.
 *
 * Returns:
 * The returned value should be equal to @len. In case
 * of an error, a negative value is returned.
 */
typedef ssize_t
(*REALTIME_CHILD_READ2_FUNC)(realtime_child_session_context *child_s_ctx,
                             size_t len);
typedef int
(*REALTIME_CHILD_RELEASE_FUNC)(realtime_child_session_context *);

struct realtime_child_funcs {
    REALTIME_CHILD_OPEN_FUNC        open;
    REALTIME_CHILD_WRITE1_FUNC      write1;
    REALTIME_CHILD_WRITE2_FUNC      write2;
    REALTIME_CHILD_EXECUTE_FUNC     execute;
    REALTIME_CHILD_READ1_FUNC       read1;
    REALTIME_CHILD_READ2_FUNC       read2;
    REALTIME_CHILD_RELEASE_FUNC     release;
};

int realtime_init(realtime_context **);
void realtime_exit(realtime_context **);
int realtime_set_hook(realtime_context *,
                      realtime_child_context *,
                      const struct realtime_child_funcs *);
/**
 * realtime_get_sysfs_attribute_group - Returns the attribute group.
 * @rt_ctx: The realtime context.
 *
 * Returns an attribute group.
 */
const struct attribute_group *realtime_get_attribute_group(realtime_context *rt_ctx);
const struct resource_manager_child_funcs *realtime_get_hook_resource_manager(void);

#endif
