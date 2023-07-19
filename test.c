#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <poll.h>

#define DEVICE_NODE "/dev/op0"

#define CMD_AIO 0x00
#define CMD_INVALID 0xFF

int test (bool nonblock) {
    int fd;
    uint8_t buffer[256];
    ssize_t len;

    if (nonblock) {
        fd = open (DEVICE_NODE, O_RDWR | O_NONBLOCK);
    } else {
        fd = open (DEVICE_NODE, O_RDWR);
    }
    if (fd == -1) {
        perror("open()");
        return 1;
    }

    /* Send command */
    buffer[0] = CMD_AIO;
    printf ("Command: 0x%02x\n", buffer[0]);

    len = write (fd, buffer, 1);
    if (len != 1) {
        perror("read()");
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
    len = read (fd, buffer, sizeof (buffer));
    if (len != 1) {
        perror("read()");
        goto out_free_fd;
    }

    printf ("Response: 0x%02x\n", buffer[0]);

    close (fd);
    return 0;

out_free_fd:
    close (fd);
    return 1;
}

int main () {
    if (test (false)) {
        return 1;
    }

    //sleep (1);

    if (test (true)) {
        return 1;
    }

    return 0;
}

