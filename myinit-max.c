#define _GNU_SOURCE
#include "myinit-max.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <signal.h>
#include <pty.h>
#include <utmp.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#ifdef DEBUG
#define DEBUG_PRINT(fmt, ...) do { \
    fprintf(stdout, LOG_PREFIX fmt, ##__VA_ARGS__); \
    fflush(stdout); \
} while (0)
#else
#define DEBUG_PRINT(fmt, ...)
#endif

// Write exactly len bytes to fd, handling partial writes and EAGAIN.
// Returns 0 on success, -1 on error.
static int write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                fd_set wfds;
                FD_ZERO(&wfds);
                FD_SET(fd, &wfds);
                if (select(fd + 1, NULL, &wfds, NULL, NULL) < 0) {
                    if (errno == EINTR)
                        continue;
                    return -1;
                }
                continue;
            }
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

// Read one chunk from fd into buf (up to len bytes).
// Returns bytes read (>0), 0 on EOF, -1 on error.
static ssize_t read_chunk(int fd, char *buf, size_t len) {
    for (;;) {
        ssize_t n = read(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(fd, &rfds);
                if (select(fd + 1, &rfds, NULL, NULL, NULL) < 0) {
                    if (errno == EINTR)
                        continue;
                    return -1;
                }
                continue;
            }
            return -1;
        }
        return n;
    }
}

// Fill a termios structure with sane defaults for an interactive console:
// canonical mode, echo, signals, correct erase/kill/eof/suspend characters.
// Used both as the initial PTY termios and as the slave termios.
static void init_termios(struct termios *term) {
    memset(term, 0, sizeof(*term));
    term->c_iflag = ICRNL | BRKINT;
    term->c_oflag = OPOST | ONLCR;
    term->c_cflag = CREAD | CS8 | B9600;
    term->c_lflag = ICANON | ECHO | ECHOE | ECHOK | ISIG | IEXTEN;

    term->c_cc[VINTR]    = 3;   // Ctrl-C
    term->c_cc[VQUIT]    = 28;  // Ctrl-backslash
    term->c_cc[VERASE]   = 127; // Backspace (DEL)
    term->c_cc[VKILL]    = 21;  // Ctrl-U
    term->c_cc[VEOF]     = 4;   // Ctrl-D
    term->c_cc[VSTART]   = 17;  // Ctrl-Q
    term->c_cc[VSTOP]    = 19;  // Ctrl-S
    term->c_cc[VSUSP]    = 26;  // Ctrl-Z
    term->c_cc[VREPRINT] = 18;  // Ctrl-R
    term->c_cc[VWERASE]  = 23;  // Ctrl-W
    term->c_cc[VLNEXT]   = 22;  // Ctrl-V
}

// Handle a single client connection.
void *handle_client(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);

    DEBUG_PRINT("Received new client connection\n");

    // Read the request from the socket (format: PID<SOH>command<SOH>cwd).
    char buffer[1024];
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        fprintf(stderr, LOG_PREFIX "Failed to read from client socket: %s\n", strerror(errno));
        close(client_fd);
        return NULL;
    }
    buffer[bytes_read] = '\0';
    DEBUG_PRINT("Received message (%zd bytes): '%s'\n", bytes_read, buffer);

    // Parse PID, command and cwd using SOH (\001) as separator.
    char *pid_str = strtok(buffer, "\001");
    char *command = strtok(NULL, "\001");
    char *cwd = strtok(NULL, "");
    if (!pid_str || !command || !cwd) {
        fprintf(stderr, LOG_PREFIX "Invalid message format, expected PID<SOH>command<SOH>cwd, got pid_str='%s', command='%s', cwd='%s'\n",
                pid_str ? pid_str : "(null)", command ? command : "(null)", cwd ? cwd : "(null)");
        close(client_fd);
        return NULL;
    }
    DEBUG_PRINT("Parsed PID='%s', command='%s', cwd='%s'\n", pid_str, command, cwd);

    // Convert PID to integer.
    pid_t client_pid = atoi(pid_str);
    if (client_pid <= 0) {
        fprintf(stderr, LOG_PREFIX "Invalid PID: %s\n", pid_str);
        close(client_fd);
        return NULL;
    }
    DEBUG_PRINT("Converted PID to %d\n", client_pid);

    // Verify that the process still exists.
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d", client_pid);
    if (access(proc_path, F_OK) != 0) {
        fprintf(stderr, LOG_PREFIX "Process with PID %d does not exist: %s\n", client_pid, strerror(errno));
        close(client_fd);
        return NULL;
    }

    // Open /proc/PID/fd/0, /proc/PID/fd/1, /proc/PID/fd/2.
    char fd_path[64];
    int stdin_fd, stdout_fd, stderr_fd;

    snprintf(fd_path, sizeof(fd_path), "/proc/%d/fd/0", client_pid);
    stdin_fd = open(fd_path, O_RDWR);
    if (stdin_fd < 0) {
        fprintf(stderr, LOG_PREFIX "Failed to open %s: %s\n", fd_path, strerror(errno));
        close(client_fd);
        return NULL;
    }
    DEBUG_PRINT("Opened %s as fd %d\n", fd_path, stdin_fd);

    snprintf(fd_path, sizeof(fd_path), "/proc/%d/fd/1", client_pid);
    stdout_fd = open(fd_path, O_WRONLY);
    if (stdout_fd < 0) {
        fprintf(stderr, LOG_PREFIX "Failed to open %s: %s\n", fd_path, strerror(errno));
        close(stdin_fd);
        close(client_fd);
        return NULL;
    }
    DEBUG_PRINT("Opened %s as fd %d\n", fd_path, stdout_fd);

    snprintf(fd_path, sizeof(fd_path), "/proc/%d/fd/2", client_pid);
    stderr_fd = open(fd_path, O_WRONLY);
    if (stderr_fd < 0) {
        fprintf(stderr, LOG_PREFIX "Failed to open %s: %s\n", fd_path, strerror(errno));
        close(stdin_fd);
        close(stdout_fd);
        close(client_fd);
        return NULL;
    }
    DEBUG_PRINT("Opened %s as fd %d\n", fd_path, stderr_fd);

    // Change working directory.
    if (chdir(cwd) < 0) {
        fprintf(stderr, LOG_PREFIX "Failed to change directory to %s: %s\n", cwd, strerror(errno));
        close(stdin_fd);
        close(stdout_fd);
        close(stderr_fd);
        close(client_fd);
        return NULL;
    }
    DEBUG_PRINT("Changed current directory to %s\n", cwd);

    // Execute the command via fork + execle.
    DEBUG_PRINT("Executing command: %s\n", command);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, LOG_PREFIX "fork() failed: %s\n", strerror(errno));
        close(stdin_fd);
        close(stdout_fd);
        close(stderr_fd);
        close(client_fd);
        return NULL;
    } else if (pid == 0) {
        // Child: restore default signal handlers so the command behaves normally.
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGHUP, SIG_DFL);

        if (dup2(stdin_fd, STDIN_FILENO) < 0) {
            fprintf(stderr, LOG_PREFIX "Failed to dup2 stdin: %s\n", strerror(errno));
            _exit(1);
        }
        DEBUG_PRINT("Redirected stdin to fd %d\n", stdin_fd);

        if (dup2(stdout_fd, STDOUT_FILENO) < 0) {
            fprintf(stderr, LOG_PREFIX "Failed to dup2 stdout: %s\n", strerror(errno));
            _exit(1);
        }
        DEBUG_PRINT("Redirected stdout to fd %d\n", stdout_fd);

        if (dup2(stderr_fd, STDERR_FILENO) < 0) {
            fprintf(stderr, LOG_PREFIX "Failed to dup2 stderr: %s\n", strerror(errno));
            _exit(1);
        }
        DEBUG_PRINT("Redirected stderr to fd %d\n", stderr_fd);

        // Close the file descriptors after redirection.
        close(stdin_fd);
        close(stdout_fd);
        close(stderr_fd);
        DEBUG_PRINT("Closed file descriptors for client PID %d\n", client_pid);

        // Run the command through execle.
        char *const envp[] = {"HOSTNAME=localhost", "TERM=dumb", NULL};
        execle("/system/bin/sh", "sh", "-c", command, NULL, envp);
        // If execle returns, an error occurred.
        fprintf(stderr, LOG_PREFIX "execle() failed to invoke /system/bin/sh: %s\n", strerror(errno));
        _exit(127);
    } else {
        // Parent: close the descriptors, wait for the command, then notify the client.
        close(stdin_fd);
        close(stdout_fd);
        close(stderr_fd);
        DEBUG_PRINT("Closed file descriptors for client PID %d in parent\n", client_pid);

        int status;
        if (waitpid(pid, &status, 0) < 0) {
            fprintf(stderr, LOG_PREFIX "waitpid() failed: %s\n", strerror(errno));
        } else if (WIFEXITED(status)) {
            DEBUG_PRINT("Command completed with exit status %d\n", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            DEBUG_PRINT("Command terminated by signal %d\n", WTERMSIG(status));
        }

        const char *done_msg = "DONE";
        if (write(client_fd, done_msg, strlen(done_msg)) < 0) {
            fprintf(stderr, LOG_PREFIX "Failed to send DONE to client: %s\n", strerror(errno));
        } else {
            DEBUG_PRINT("Sent DONE to client\n");
        }

        close(client_fd);
        DEBUG_PRINT("Client connection closed\n");
    }

    return NULL;
}

// Listen for commands over MYINIT_SOCKET_PATH.
void command_listener(void) {
    const char *socket_path = MYINIT_SOCKET_PATH;
    int server_fd;
    struct sockaddr_un addr;

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        fprintf(stderr, LOG_PREFIX "Failed to create socket: %s\n", strerror(errno));
        exit(1);
    }
    DEBUG_PRINT("Created socket fd %d\n", server_fd);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    unlink(socket_path);
    DEBUG_PRINT("Removed existing socket at %s\n", socket_path);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, LOG_PREFIX "Failed to bind socket %s: %s\n", socket_path, strerror(errno));
        close(server_fd);
        exit(1);
    }
    DEBUG_PRINT("Bound socket to %s\n", socket_path);

    if (chmod(socket_path, 0666) < 0) {
        fprintf(stderr, LOG_PREFIX "Failed to set permissions on %s: %s\n", socket_path, strerror(errno));
        unlink(socket_path);
        close(server_fd);
        exit(1);
    }
    DEBUG_PRINT("Set permissions 0666 on %s\n", socket_path);

    if (listen(server_fd, 10) < 0) {
        fprintf(stderr, LOG_PREFIX "Failed to listen on socket: %s\n", strerror(errno));
        unlink(socket_path);
        close(server_fd);
        exit(1);
    }
    DEBUG_PRINT("Listening on socket\n");

    while (1) {
        int *client_fd = malloc(sizeof(int));
        if (!client_fd) {
            fprintf(stderr, LOG_PREFIX "Failed to allocate memory for client_fd\n");
            continue;
        }

        *client_fd = accept(server_fd, NULL, NULL);
        if (*client_fd < 0) {
            fprintf(stderr, LOG_PREFIX "Failed to accept connection: %s\n", strerror(errno));
            free(client_fd);
            continue;
        }
        DEBUG_PRINT("Accepted client connection, fd %d\n", *client_fd);

        pthread_t client_thread;
        if (pthread_create(&client_thread, NULL, handle_client, client_fd) != 0) {
            fprintf(stderr, LOG_PREFIX "Failed to create client thread: %s\n", strerror(errno));
            close(*client_fd);
            free(client_fd);
            continue;
        }

        pthread_detach(client_thread);
        DEBUG_PRINT("Detached client thread for fd %d\n", *client_fd);
    }

    close(server_fd);
    unlink(socket_path);
    DEBUG_PRINT("Closed server socket and unlinked %s\n", socket_path);
}

// Start the command listener in a forked child.
void start_command_listener(void) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, LOG_PREFIX "Failed to fork for command listener: %s\n", strerror(errno));
        return;
    } else if (pid == 0) {
        DEBUG_PRINT("Started command listener in child process with PID %d\n", getpid());
        command_listener();
        exit(0);
    } else {
        DEBUG_PRINT("Forked command listener with PID %d\n", pid);
    }
}

// Check whether a given mount point is already mounted.
int check_mount(const char *mounts_file, const char *mount_point) {
    FILE *proc_mounts = fopen(mounts_file, "r");
    if (!proc_mounts) {
        fprintf(stderr, LOG_PREFIX "Failed to open %s: %s\n", mounts_file, strerror(errno));
        return 0;
    }

    char line[256];
    int mounted = 0;
    while (fgets(line, sizeof(line), proc_mounts)) {
        line[strcspn(line, "\n")] = '\0';
        if (strstr(line, mount_point)) {
            mounted = 1;
            break;
        }
    }
    fclose(proc_mounts);
    return mounted;
}

// Main init logic: start listener, wait for /data, run script, launch interactive shell.
void handle_logic() {
    // 15 seconds should be enough for the initial boot phase.
    sleep(15);

#if START_COMMAND_LISTENER
    start_command_listener();
#else
    DEBUG_PRINT("Command listener disabled by START_COMMAND_LISTENER=0\n");
#endif

    // Wait for /data to be mounted.
    DEBUG_PRINT("Waiting for /data mount\n");
    while (1) {
        sleep(1);
        if (check_mount("/proc/1/mounts", " /data ")) {
            break;
        }
    }

    DEBUG_PRINT("Starting %s\n", MYSCRIPT_PATH);
    char script_cmd[512];
    snprintf(script_cmd, sizeof(script_cmd), "sh %s", MYSCRIPT_PATH);
    int script_ret = system(script_cmd);
    if (script_ret == -1) {
        fprintf(stderr, LOG_PREFIX "Failed to execute %s: %s\n", MYSCRIPT_PATH, strerror(errno));
    } else {
        fprintf(stderr, LOG_PREFIX "%s completed with exit status %d\n", MYSCRIPT_PATH, WEXITSTATUS(script_ret));
    }

    DEBUG_PRINT("Starting interactive shell on console\n");

    // Make sure stdin is a real terminal. If not, attach to /dev/console.
    // The resulting termios is not trusted; the PTY is always initialised
    // explicitly below.
    if (!isatty(STDIN_FILENO)) {
        int cfd = open("/dev/console", O_RDWR);
        if (cfd >= 0) {
            dup2(cfd, STDIN_FILENO);
            dup2(cfd, STDOUT_FILENO);
            dup2(cfd, STDERR_FILENO);
            if (cfd > STDERR_FILENO) close(cfd);
            DEBUG_PRINT("Attached stdio to /dev/console\n");
        } else {
            fprintf(stderr, LOG_PREFIX "Cannot open /dev/console: %s\n", strerror(errno));
        }
    }

    // Get terminal window size, if possible.
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == -1) {
        memset(&ws, 0, sizeof(ws));
        ws.ws_row = 24;
        ws.ws_col = 80;
    }

    // Build an explicit termios for the PTY. Passing NULL to forkpty()
    // would leave the slave with whatever (possibly non-terminal) termios
    // was inherited, which breaks ICANON/ISIG/VERASE (backspace, Ctrl-C,
    // Ctrl-Z, etc.). We pass our own initialised struct instead.
    struct termios pty_term;
    init_termios(&pty_term);

    // forkpty() creates a master/slave PTY pair, calls setsid() and
    // login_tty() in the child so the slave becomes the controlling
    // terminal.
    int master_fd;
    pid_t pid = forkpty(&master_fd, NULL, &pty_term, &ws);
    if (pid == -1) {
        fprintf(stderr, LOG_PREFIX "forkpty failed: %s\n", strerror(errno));
        exit(1);
    }

    if (pid == 0) {
        // Child: forkpty already did setsid() + login_tty(), so /dev/tty
        // is usable. Re-apply the sane termios on the slave explicitly
        // in case the kernel reset anything, then exec the shell.
        struct termios term;
        init_termios(&term);
        tcsetattr(STDIN_FILENO, TCSANOW, &term);

        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);

        char *const envp[] = {
            "HOSTNAME=localhost",
            "TERM=linux",
            "PATH=/bin:/sbin:/system/bin:/system/xbin:/system/sbin:/odm/bin:/system/vendor/bin:/product/bin:/vendor/bin:/vendor/xbin",
            NULL
        };
        execle("/system/bin/sh", "sh", "-i", NULL, envp);
        fprintf(stderr, LOG_PREFIX "Failed to exec /system/bin/sh: %s\n", strerror(errno));
        _exit(127);
    }

    // Parent: do NOT touch termios on master_fd. Termios on the master
    // side of a PTY does not affect the slave's line discipline; the
    // child has already configured the slave correctly.
    //
    // Keep the master blocking. select() tells us when data is ready,
    // and write_all()/read_chunk() handle EINTR/EAGAIN and partial I/O.
    // This avoids losing input during fast typing or paste, which was
    // caused by setting O_NONBLOCK and bailing out on EAGAIN.
    fd_set read_fds;
    int max_fd = (master_fd > STDIN_FILENO) ? master_fd : STDIN_FILENO;
    char buffer[4096];
    int running = 1;

    while (running) {
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        FD_SET(master_fd, &read_fds);

        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            ssize_t n = read_chunk(STDIN_FILENO, buffer, sizeof(buffer));
            if (n <= 0) {
                running = 0;
                break;
            }
            if (write_all(master_fd, buffer, (size_t)n) < 0) {
                running = 0;
                break;
            }
        }

        if (FD_ISSET(master_fd, &read_fds)) {
            ssize_t n = read_chunk(master_fd, buffer, sizeof(buffer));
            if (n <= 0) {
                running = 0;
                break;
            }
            if (write_all(STDOUT_FILENO, buffer, (size_t)n) < 0) {
                running = 0;
                break;
            }
        }
    }

    close(master_fd);
    int status;
    waitpid(pid, &status, 0);

    exit(0);
}

// Mount sysfs and, optionally, configure SELinux enforce mode.
void setup_filesystems(void) {
    if (mount("sys", "/sys", "sysfs", 0, NULL) != 0) {
        perror(LOG_PREFIX "Failed to mount sysfs on /sys");
        exit(EXIT_FAILURE);
    }
    DEBUG_PRINT("Successfully mounted sysfs on /sys\n");

#if DISABLE_SELINUX_ENFORCE
    if (mount("selinuxfs", "/sys/fs/selinux", "selinuxfs", 0, NULL) != 0) {
        perror(LOG_PREFIX "Failed to mount selinuxfs on /sys/fs/selinux");
        exit(EXIT_FAILURE);
    }
    DEBUG_PRINT("Successfully mounted selinuxfs on /sys/fs/selinux\n");

    int fd = open("/dev/enforce", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        perror(LOG_PREFIX "Failed to open /dev/enforce");
        exit(EXIT_FAILURE);
    }

    if (write(fd, "1", 1) != 1) {
        perror(LOG_PREFIX "Failed to write to /dev/enforce");
        close(fd);
        exit(EXIT_FAILURE);
    }
    DEBUG_PRINT("Successfully wrote '1' to /dev/enforce\n");
    close(fd);

    if (mount("/dev/enforce", "/sys/fs/selinux/enforce", NULL, MS_BIND, NULL) != 0) {
        perror(LOG_PREFIX "Failed to bind mount /dev/enforce to /sys/fs/selinux/enforce");
        exit(EXIT_FAILURE);
    }
    DEBUG_PRINT("Successfully bind mounted /dev/enforce to /sys/fs/selinux/enforce\n");
#else
    DEBUG_PRINT("SELinux enforce setup skipped by DISABLE_SELINUX_ENFORCE=0\n");
#endif
}

int main() {
    signal(SIGTERM, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    setup_filesystems();

    pid_t pid = fork();

    if (pid == -1) {
        perror(LOG_PREFIX "fork failed");
        exit(1);
    }
    DEBUG_PRINT("Forked, child PID %d\n", pid);

    if (pid == 0) {
        handle_logic();
    } else {
        execl(TARGET_PROGRAM, TARGET_PROGRAM, NULL);
        perror(LOG_PREFIX "execl failed");
        exit(1);
    }

    return 0;
}
