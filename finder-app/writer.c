#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    openlog("writer", LOG_PID, LOG_USER);

    if (argc != 3) {
        syslog(LOG_ERR, "Invalid number of arguments: expected 2, got %d",
               argc - 1);
        fprintf(stderr, "Error: Two arguments required\n");
        fprintf(stderr, "Usage: writer <writefile> <writestr>\n");
        closelog();
        return 1;
    }

    const char *writefile = argv[1];
    const char *writestr = argv[2];

    syslog(LOG_DEBUG, "Writing %s to %s", writestr, writefile);

    int fd = open(writefile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd == -1) {
        syslog(LOG_ERR, "Failed to open file %s", writefile);
        fprintf(stderr, "Error: Could not create file %s\n", writefile);
        closelog();
        return 1;
    }

    ssize_t len = (ssize_t)strlen(writestr);
    ssize_t written = write(fd, writestr, len);
    if (written != len) {
        syslog(LOG_ERR, "Failed to write to file %s", writefile);
        fprintf(stderr, "Error: Could not write to file %s\n", writefile);
        close(fd);
        closelog();
        return 1;
    }

    if (write(fd, "\n", 1) != 1) {
        syslog(LOG_ERR, "Failed to write newline to file %s", writefile);
        close(fd);
        closelog();
        return 1;
    }

    close(fd);
    closelog();
    return 0;
}
