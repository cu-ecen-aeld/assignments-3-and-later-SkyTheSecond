#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#define PORT "9000"
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BACKLOG 10
#define RECV_BUF_SIZE 1024

/* Globals needed by the signal handler so it can clean up. */
static volatile sig_atomic_t g_exit_requested = 0;
static int g_listen_fd = -1;
static int g_client_fd = -1;

/*
 * Signal handler for SIGINT / SIGTERM.
 *
 * Kept minimal and signal-safe: just sets a flag and (best-effort)
 * shuts down any open sockets so blocking accept()/recv() calls
 * return with an error instead of hanging forever. The real cleanup
 * (closing fds, removing the data file, syslog message) happens in
 * main() after the loop notices the flag.
 */
static void signal_handler(int signo) {
    (void)signo;
    g_exit_requested = 1;

    /* Force any blocked accept()/recv() to return immediately. */
    if (g_listen_fd != -1) {
        shutdown(g_listen_fd, SHUT_RDWR);
    }
    if (g_client_fd != -1) {
        shutdown(g_client_fd, SHUT_RDWR);
    }
}

/*
 * Set up SIGINT and SIGTERM handlers using sigaction (preferred over
 * signal() because it lets us avoid SA_RESTART surprises and is
 * portable/well-defined).
 */
static int setup_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags =
        0; /* no SA_RESTART: we want accept()/recv() to fail with EINTR */

    if (sigaction(SIGINT, &sa, NULL) != 0) {
        syslog(LOG_ERR, "sigaction(SIGINT) failed: %s", strerror(errno));
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        syslog(LOG_ERR, "sigaction(SIGTERM) failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/*
 * Create, bind, and start listening on a TCP stream socket for the
 * given port. Returns the listening socket fd, or -1 on failure.
 */
static int create_and_bind_socket(const char *port) {
    struct addrinfo hints, *servinfo = NULL, *p;
    int sockfd = -1;
    int rv;
    int yes = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     /* IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM; /* TCP stream socket */
    hints.ai_flags = AI_PASSIVE;     /* use my IP / wildcard bind */

    rv = getaddrinfo(NULL, port, &hints, &servinfo);
    if (rv != 0) {
        syslog(LOG_ERR, "getaddrinfo failed: %s", gai_strerror(rv));
        return -1;
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd == -1) {
            syslog(LOG_ERR, "socket() failed: %s", strerror(errno));
            continue;
        }

        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) ==
            -1) {
            syslog(LOG_ERR, "setsockopt(SO_REUSEADDR) failed: %s",
                   strerror(errno));
            close(sockfd);
            sockfd = -1;
            continue;
        }

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            syslog(LOG_ERR, "bind() failed: %s", strerror(errno));
            close(sockfd);
            sockfd = -1;
            continue;
        }

        break; /* success */
    }

    freeaddrinfo(servinfo);

    if (sockfd == -1) {
        syslog(LOG_ERR, "Failed to bind to port %s", port);
        return -1;
    }

    if (listen(sockfd, BACKLOG) == -1) {
        syslog(LOG_ERR, "listen() failed: %s", strerror(errno));
        close(sockfd);
        return -1;
    }

    return sockfd;
}

/*
 * Extract a human readable IP address string from a sockaddr_storage
 * (handles both IPv4 and IPv6) into the provided buffer.
 */
static void get_ip_str(struct sockaddr_storage *addr, char *buf,
                       size_t buflen) {
    if (addr->ss_family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)addr;
        inet_ntop(AF_INET, &s->sin_addr, buf, buflen);
    } else if (addr->ss_family == AF_INET6) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)addr;
        inet_ntop(AF_INET6, &s->sin6_addr, buf, buflen);
    } else {
        snprintf(buf, buflen, "unknown");
    }
}

/*
 * Append `len` bytes from `data` to the data file. Returns 0 on
 * success, -1 on failure.
 */
static int append_to_data_file(const char *data, size_t len) {
    int fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd == -1) {
        syslog(LOG_ERR, "open(%s) for append failed: %s", DATA_FILE,
               strerror(errno));
        return -1;
    }

    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            syslog(LOG_ERR, "write() to %s failed: %s", DATA_FILE,
                   strerror(errno));
            close(fd);
            return -1;
        }
        written += (size_t)n;
    }

    close(fd);
    return 0;
}

/*
 * Send the full contents of the data file to the given socket.
 * Returns 0 on success, -1 on failure.
 */
static int send_data_file(int client_fd) {
    int fd = open(DATA_FILE, O_RDONLY);
    if (fd == -1) {
        syslog(LOG_ERR, "open(%s) for read failed: %s", DATA_FILE,
               strerror(errno));
        return -1;
    }

    char buf[RECV_BUF_SIZE];
    ssize_t n;
    int ret = 0;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t sent = 0;
        while (sent < n) {
            ssize_t s = send(client_fd, buf + sent, (size_t)(n - sent), 0);
            if (s == -1) {
                if (errno == EINTR) {
                    continue;
                }
                syslog(LOG_ERR, "send() failed: %s", strerror(errno));
                ret = -1;
                goto out;
            }
            sent += s;
        }
    }
    if (n == -1) {
        syslog(LOG_ERR, "read(%s) failed: %s", DATA_FILE, strerror(errno));
        ret = -1;
    }

out:
    close(fd);
    return ret;
}

/*
 * Handle a single client connection:
 *  - Receive bytes into a dynamically growing buffer.
 *  - Whenever a newline is seen, the bytes up to and including the
 *    newline form a "packet": append it to the data file, then send
 *    the full file content back to the client.
 *  - Keep going (supporting multiple packets per connection, and
 *    partial packets split across recv() calls) until the client
 *    closes the connection or we are asked to exit.
 */
static void handle_client(int client_fd) {
    char recv_buf[RECV_BUF_SIZE];

    /* Dynamically growing buffer that accumulates a single packet
     * until we see a newline. malloc() failures are handled by
     * discarding the in-progress packet and logging an error, per
     * the assignment's allowance to drop over-length packets. */
    char *packet_buf = NULL;
    size_t packet_len = 0;
    size_t packet_cap = 0;

    while (!g_exit_requested) {
        ssize_t n = recv(client_fd, recv_buf, sizeof(recv_buf), 0);
        if (n == 0) {
            /* Client closed the connection. */
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            syslog(LOG_ERR, "recv() failed: %s", strerror(errno));
            break;
        }

        size_t start = 0;
        for (size_t i = 0; i < (size_t)n; i++) {
            if (recv_buf[i] == '\n') {
                /* Found end of a packet: copy [start, i] inclusive
                 * of the newline into packet_buf, then process it. */
                size_t chunk_len = i - start + 1;

                if (packet_len + chunk_len > packet_cap) {
                    size_t new_cap =
                        (packet_cap == 0) ? RECV_BUF_SIZE : packet_cap * 2;
                    while (new_cap < packet_len + chunk_len) {
                        new_cap *= 2;
                    }
                    char *new_buf = realloc(packet_buf, new_cap);
                    if (new_buf == NULL) {
                        syslog(LOG_ERR,
                               "malloc/realloc failed while assembling packet "
                               "(%zu bytes requested): %s. Discarding packet.",
                               new_cap, strerror(errno));
                        free(packet_buf);
                        packet_buf = NULL;
                        packet_len = 0;
                        packet_cap = 0;
                        /* Skip past this newline and keep scanning the
                         * rest of recv_buf for further packets. */
                        start = i + 1;
                        continue;
                    }
                    packet_buf = new_buf;
                    packet_cap = new_cap;
                }

                memcpy(packet_buf + packet_len, recv_buf + start, chunk_len);
                packet_len += chunk_len;

                /* Complete packet assembled: append then respond. */
                if (append_to_data_file(packet_buf, packet_len) == -1) {
                    syslog(LOG_ERR, "Failed to append packet to data file");
                } else {
                    if (send_data_file(client_fd) == -1) {
                        syslog(LOG_ERR,
                               "Failed to send data file back to client");
                    }
                }

                packet_len = 0; /* reset for next packet, keep capacity */
                start = i + 1;
            }
        }

        /* Any leftover bytes after the last newline (partial packet)
         * need to be carried over to the next recv() call. */
        size_t leftover_len = (size_t)n - start;
        if (leftover_len > 0) {
            if (packet_len + leftover_len > packet_cap) {
                size_t new_cap =
                    (packet_cap == 0) ? RECV_BUF_SIZE : packet_cap * 2;
                while (new_cap < packet_len + leftover_len) {
                    new_cap *= 2;
                }
                char *new_buf = realloc(packet_buf, new_cap);
                if (new_buf == NULL) {
                    syslog(LOG_ERR,
                           "malloc/realloc failed while assembling packet "
                           "(%zu bytes requested): %s. Discarding packet.",
                           new_cap, strerror(errno));
                    free(packet_buf);
                    packet_buf = NULL;
                    packet_len = 0;
                    packet_cap = 0;
                    continue;
                }
                packet_buf = new_buf;
                packet_cap = new_cap;
            }
            memcpy(packet_buf + packet_len, recv_buf + start, leftover_len);
            packet_len += leftover_len;
        }
    }

    free(packet_buf);
}

/*
 * Daemonize the current process using the standard double-fork-free
 * approach (single fork is sufficient for this assignment's needs):
 *  - fork()
 *  - parent exits
 *  - child calls setsid(), chdir("/"), redirects std fds to /dev/null
 *
 * Returns 0 in the child (continuing process), exits the process
 * directly in the parent. Returns -1 on failure (caller should exit).
 */
static int daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "fork() failed: %s", strerror(errno));
        return -1;
    }
    if (pid > 0) {
        /* Parent: exit successfully, daemon continues in child. */
        exit(EXIT_SUCCESS);
    }

    /* Child continues here. */
    if (setsid() == -1) {
        syslog(LOG_ERR, "setsid() failed: %s", strerror(errno));
        return -1;
    }

    if (chdir("/") == -1) {
        syslog(LOG_ERR, "chdir(\"/\") failed: %s", strerror(errno));
        return -1;
    }

    int devnull = open("/dev/null", O_RDWR);
    if (devnull == -1) {
        syslog(LOG_ERR, "open(/dev/null) failed: %s", strerror(errno));
        return -1;
    }
    dup2(devnull, STDIN_FILENO);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    if (devnull > STDERR_FILENO) {
        close(devnull);
    }

    return 0;
}

int main(int argc, char *argv[]) {
    int run_as_daemon = 0;
    int opt;

    while ((opt = getopt(argc, argv, "d")) != -1) {
        switch (opt) {
        case 'd':
            run_as_daemon = 1;
            break;
        default:
            fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
            return -1;
        }
    }

    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);

    if (setup_signal_handlers() == -1) {
        closelog();
        return -1;
    }

    /* Bind/listen BEFORE forking, so daemon mode can report bind
     * failures to the parent's exit status / stderr before detaching. */
    g_listen_fd = create_and_bind_socket(PORT);
    if (g_listen_fd == -1) {
        syslog(LOG_ERR, "Failed to create and bind socket on port %s", PORT);
        closelog();
        return -1;
    }

    if (run_as_daemon) {
        if (daemonize() == -1) {
            close(g_listen_fd);
            closelog();
            return -1;
        }
        /* Only the child (daemon) process reaches here. */
    }

    /* Main accept loop. */
    while (!g_exit_requested) {
        struct sockaddr_storage client_addr;
        socklen_t client_addr_len = sizeof(client_addr);

        int client_fd = accept(g_listen_fd, (struct sockaddr *)&client_addr,
                               &client_addr_len);
        if (client_fd == -1) {
            if (g_exit_requested) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            syslog(LOG_ERR, "accept() failed: %s", strerror(errno));
            continue;
        }

        g_client_fd = client_fd;

        char ip_str[INET6_ADDRSTRLEN];
        get_ip_str(&client_addr, ip_str, sizeof(ip_str));
        syslog(LOG_INFO, "Accepted connection from %s", ip_str);

        handle_client(client_fd);

        syslog(LOG_INFO, "Closed connection from %s", ip_str);
        close(client_fd);
        g_client_fd = -1;
    }

    /* Graceful shutdown: caught SIGINT/SIGTERM. */
    syslog(LOG_INFO, "Caught signal, exiting");

    if (g_listen_fd != -1) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }

    if (remove(DATA_FILE) == -1 && errno != ENOENT) {
        syslog(LOG_ERR, "Failed to remove %s: %s", DATA_FILE, strerror(errno));
    }

    closelog();
    return 0;
}
