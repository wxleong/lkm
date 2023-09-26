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
 * @file   kunit.c
 * @date   July, 2023
 * @brief  KUnit test for the kernel module.
 */

#include <kunit/test.h>
#include "lkm.h"

static void chardev_lkm_add_test_basic(struct kunit *test)
{
        KUNIT_EXPECT_EQ(test, 0xD073D74C, LKM_CONTEXT_MAGIC);
}

static void chardev_lkm_test_failure(struct kunit *test)
{
        KUNIT_FAIL(test, "This test is expected to fail.");
}

static struct kunit_case chardev_lkm_test_cases[] = {
        KUNIT_CASE(chardev_lkm_add_test_basic),
        KUNIT_CASE(chardev_lkm_test_failure),
        {}
};

static struct kunit_suite chardev_lkm_test_suite = {
        .name = "chardev-lkm-test",
        .test_cases = chardev_lkm_test_cases,
};
kunit_test_suite(chardev_lkm_test_suite);

MODULE_LICENSE("GPL");
