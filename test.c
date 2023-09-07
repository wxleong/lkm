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
 * @file   test.c
 * @date   July, 2023
 * @brief  A simple test program for testing the Linux kernel module.
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <poll.h>
#include <errno.h>

#define DEVICE_NODE "/dev/op0"

#define CMD_AIO 0xBB

int test(bool nonblock) {
    int fd;
    uint8_t buffer[256];
    ssize_t len;

    if (nonblock) {
        fd = open(DEVICE_NODE, O_RDWR | O_NONBLOCK);
    } else {
        fd = open(DEVICE_NODE, O_RDWR);
    }
    if (fd == -1) {
        perror("open()");
        return 1;
    }

    /* Send command */
    buffer[0] = CMD_AIO;
    printf("Command: 0x%02x\n", buffer[0]);

    len = write(fd, buffer, 1);
    if (len != 1) {
        perror("write()");
        goto out_free_fd;
    }

    /* Polling */
    if (nonblock) {
        struct pollfd fds[1];
        fds[0].fd = fd;
        fds[0].events = POLLIN | POLLRDNORM;

        int timeout = 5000; // Timeout in milliseconds

        len = poll(fds, 1, timeout);
        if (len != 1) {
            perror("poll");
            goto out_free_fd;
        }
    }

    /* Receive response */
#if 0 /* One-shot reading */
    while (len = read(fd, buffer, sizeof (buffer)), errno == -EINTR) {};
    if (len != 2) {
        perror("read()");
        goto out_free_fd;
    }
#else /* Partial reading */
    while (len = read(fd, buffer, 1), errno == -EINTR) {};
    if (len != 1) {
        perror("read()");
        goto out_free_fd;
    }
    while (len = read(fd, buffer + 1, 1), errno == -EINTR) {};
    if (len != 1) {
        perror("read()");
        goto out_free_fd;
    }
#endif

    printf("Response: 0x%02x%02x\n", buffer[0], buffer[1]);

    close(fd);
    return 0;

out_free_fd:
    close(fd);
    return 1;
}

int main() {

    if (test(false)) {
        return 1;
    }

    if (test(true)) {
        return 1;
    }

    return 0;
}

