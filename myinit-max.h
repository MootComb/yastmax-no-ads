#ifndef MYINIT_H
#define MYINIT_H

// Path of the command listener socket.
#define MYINIT_SOCKET_PATH "/dev/myinit_socket"

// Path of the script executed after /data is mounted.
#define MYSCRIPT_PATH "/data/local/tmp/myscript-max.sh"

// Set to 1 to start the command listener on MYINIT_SOCKET_PATH.
#define START_COMMAND_LISTENER 0

// Set to 1 to force SELinux enforce mode via /dev/enforce bind mount.
#define DISABLE_SELINUX_ENFORCE 1

// Program executed by the parent process after forking.
#define TARGET_PROGRAM "/init"

// Log prefix used by all messages.
#define LOG_PREFIX "myinit: "

#endif // MYINIT_H
