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
 * @file   resource-manager.h
 * @date   July, 2023
 * @brief  Header file
 *
 *         The term "orderly" here refers to an operation being processed sequentially.
 *         When an operation is marked as orderly, it is encapsulated within a work
 *         item and placed into a same single-threaded workqueue. These work items are
 *         subsequently processed one after another in a sequential order, eliminating
 *         concerns related to race conditions and synchronization. This approach is
 *         beneficial when sharing a hardware resource.
 *
 *         The following are the guarantees that the resource manager can provide:
 *           - Multiple sessions are supported. Each session has its own session context
 *             and has access to a globally shared global context.
 *           - When an operation is marked as orderly, it behaves as described above.
 *           - All functions in resource_manager_child_funcs are executed atomically.
 *           - From a session perspective, the functions in resource_manager_child_funcs
 *             are invoked in the following sequence:
 *
 *             open -> write1 -> write2 -> execute -> read1 -> read2
 *                        \                                      /
 *                         -------------------<<-----------------
 *
 *             open -> *any* -> release
 */
#ifndef __RESOURCE_MANAGER_H__
#define __RESOURCE_MANAGER_H__

#include "chardev.h"

#define RESOURCE_MANAGER_CONTEXT_MAGIC            0xF05907DA
#define RESOURCE_MANAGER_SESSION_CONTEXT_MAGIC    0x88011E12

typedef struct common_context resource_manager_context;
typedef struct common_context resource_manager_session_context;

typedef struct common_context resource_manager_child_context;
typedef struct common_context resource_manager_child_session_context;

/**
 * typedef RESOURCE_MANAGER_CHILD_OPEN_FUNC - Open a new child session.
 * @child_ctx: Child global context.
 * @child_s_ctx: A new child session context is returned here.
 * @is_orderly: A flag for receiving and sending requests for orderly execution of the callee.
 *
 * This function is initially invoked by the caller with @is_orderly set to false,
 * followed by a conditional second invocation.
 *
 * If the caller sets @is_orderly to false, it indicates that non-orderly execution
 * is in progress, and this corresponds to the initial invocation. If the caller
 * sets @is_orderly to false and the callee returns with @is_orderly untouched,
 * the second invocation will not occur. If the caller sets @is_orderly to false
 * and the callee returns with @is_orderly set to true, it indicates that an orderly
 * execution has been requested, consequently leading to the occurrence of the
 * second invocation.
 *
 * If the caller sets @is_orderly to true, it indicates that orderly execution is
 * in progress, which also implies that this is the second invocation.
 *
 * Returns:
 * On success, 0 is returned. In case of an error, a negative value is returned.
 */
typedef int
(*RESOURCE_MANAGER_CHILD_OPEN_FUNC)(resource_manager_child_context *child_ctx,
                                    resource_manager_child_session_context **child_s_ctx,
                                    bool *is_orderly);
/**
 * typedef RESOURCE_MANAGER_CHILD_EXECUTE_FUNC - Process the data received from the write operation.
 * @child_s_ctx: Session context.
 * @is_orderly: A flag for receiving and sending requests for orderly execution.
 *
 * This function is initially invoked by the caller with @is_orderly set to false,
 * followed by a conditional second invocation.
 *
 * If the caller sets @is_orderly to false, it indicates that non-orderly execution
 * is in progress, and this corresponds to the initial invocation. If the caller
 * sets @is_orderly to false and the callee returns with @is_orderly untouched,
 * the second invocation will not occur. If the caller sets @is_orderly to false
 * and the callee returns with @is_orderly set to true, it indicates that an orderly
 * execution has been requested, consequently leading to the occurrence of the
 * second invocation.
 *
 * If the caller sets @is_orderly to true, it indicates that orderly execution is
 * in progress, which also implies that this is the second invocation.
 *
 * Returns:
 * On success, 0 is returned. In case of an error, a negative value is returned.
 */
typedef int
(*RESOURCE_MANAGER_CHILD_EXECUTE_FUNC)(resource_manager_child_session_context *child_s_ctx,
                                       bool *is_orderly);
/**
 * typedef RESOURCE_MANAGER_CHILD_READ1_FUNC - Retrieve the read buffer.
 * @child_s_ctx: Session context.
 * @buf: A buffer pointer will be returned here for reading.
 *
 * Returns the size of data that can be read from @buf. In case of an error,
 * a negative value is returned.
 */
typedef int
(*RESOURCE_MANAGER_CHILD_READ1_FUNC)(resource_manager_child_session_context *child_s_ctx,
                                     char **buf);
/**
 * typedef RESOURCE_MANAGER_CHILD_READ2_FUNC - Update the progress of a reading operation.
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
typedef int
(*RESOURCE_MANAGER_CHILD_READ2_FUNC)(resource_manager_child_session_context *child_s_ctx,
                                     size_t len);
/**
 * typedef RESOURCE_MANAGER_CHILD_WRITE1_FUNC - Retrieve the write buffer.
 * @child_s_ctx: Session context.
 * @buf: A buffer pointer will be returned here for writing.
 *
 * The @buf can be written to freely prior to calling _WRITE2_FUNC.
 *
 * Returns the size of @buf. In case of an error, a negative
 * value is returned.
 */
typedef ssize_t
(*RESOURCE_MANAGER_CHILD_WRITE1_FUNC)(resource_manager_child_session_context *child_s_ctx,
                                      char **buf);
/**
 * typedef RESOURCE_MANAGER_CHILD_WRITE2_FUNC - Set the size of the data written into the write buffer..
 * @child_s_ctx: Session context.
 * @len: The number of bytes that have been written by caller.
 *
 * Returns:
 * The returned value should be equal to @len. 0 indicates no action
 * is performed. In case of an error, a negative value is returned.
 */
typedef ssize_t
(*RESOURCE_MANAGER_CHILD_WRITE2_FUNC)(resource_manager_child_session_context *child_s_ctx,
                                      size_t len);
/**
 * typedef RESOURCE_MANAGER_CHILD_RELEASE_FUNC - Release a child session.
 * @child_s_ctx: Session context.
 * @is_orderly: A flag for receiving and sending requests for orderly execution of the callee.
 *
 * This function is initially invoked by the caller with @is_orderly set to false,
 * followed by a conditional second invocation.
 *
 * If the caller sets @is_orderly to false, it indicates that non-orderly execution
 * is in progress, and this corresponds to the initial invocation. If the caller
 * sets @is_orderly to false and the callee returns with @is_orderly untouched,
 * the second invocation will not occur. If the caller sets @is_orderly to false
 * and the callee returns with @is_orderly set to true, it indicates that an orderly
 * execution has been requested, consequently leading to the occurrence of the
 * second invocation.
 *
 * If the caller sets @is_orderly to true, it indicates that orderly execution is
 * in progress, which also implies that this is the second invocation.
 *
 * Returns:
 * On success, 0 is returned. In case of an error, a negative value is returned.
 */
typedef int
(*RESOURCE_MANAGER_CHILD_RELEASE_FUNC)(resource_manager_child_session_context *child_s_ctx,
                                       bool *is_orderly);

struct resource_manager_child_funcs {
    RESOURCE_MANAGER_CHILD_OPEN_FUNC              open;

    RESOURCE_MANAGER_CHILD_WRITE1_FUNC            write1; /* To retrieve write buffer */
    RESOURCE_MANAGER_CHILD_WRITE2_FUNC            write2; /* To notify the progress of write */

    RESOURCE_MANAGER_CHILD_EXECUTE_FUNC           execute; /* To execute the data received from the write operation */

    RESOURCE_MANAGER_CHILD_READ1_FUNC             read1; /* To retrieve read buffer */
    RESOURCE_MANAGER_CHILD_READ2_FUNC             read2; /* To notify the progress of read */

    RESOURCE_MANAGER_CHILD_RELEASE_FUNC           release;
};

int resource_manager_init(resource_manager_context **);
void resource_manager_exit(resource_manager_context **);
int resource_manager_set_hook(resource_manager_context *,
                              resource_manager_child_context *,
                              const struct resource_manager_child_funcs *);
/**
 * resource_manager_get_sysfs_attribute_group - Returns the attribute group.
 * @rt_ctx: The resource_manager context.
 *
 * Returns an attribute group.
 */
const struct attribute_group *resource_manager_get_attribute_group(resource_manager_context *rt_ctx);
const struct chardev_child_funcs *resource_manager_get_hook_chardev(void);

#endif
