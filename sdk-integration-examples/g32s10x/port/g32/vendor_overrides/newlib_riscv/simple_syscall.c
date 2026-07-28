#include <errno.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <driver/console.h>

void _exit(int exit_status)
{
    (void)exit_status;
    for (;;) {
    }
}

int _kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
    errno = ENOSYS;
    return -1;
}

int _getpid(void)
{
    return 1;
}

int _open(const char *name, int flags, int mode)
{
    (void)name;
    (void)flags;
    (void)mode;
    errno = ENOSYS;
    return -1;
}

ssize_t _write(int file, const void *data, size_t length)
{
    const char *bytes = data;
    size_t index;

    if (file != STDOUT_FILENO && file != STDERR_FILENO) {
        errno = EBADF;
        return -1;
    }
    for (index = 0U; index < length; ++index) {
        console_put_char(bytes[index]);
    }
    return (ssize_t)length;
}

ssize_t _read(int file, void *data, size_t length)
{
    char *bytes = data;
    size_t index;

    if (file != STDIN_FILENO) {
        errno = EBADF;
        return -1;
    }
    for (index = 0U; index < length; ++index) {
        bytes[index] = console_get_char();
    }
    return (ssize_t)length;
}

off_t _lseek(int file, off_t offset, int origin)
{
    (void)file;
    (void)offset;
    (void)origin;
    errno = ESPIPE;
    return (off_t)-1;
}

int _close(int file)
{
    if (file == STDIN_FILENO || file == STDOUT_FILENO ||
        file == STDERR_FILENO) {
        return 0;
    }
    errno = EBADF;
    return -1;
}

int _fstat(int file, struct stat *status)
{
    if (status == NULL || (file != STDIN_FILENO && file != STDOUT_FILENO &&
                           file != STDERR_FILENO)) {
        errno = EBADF;
        return -1;
    }
    status->st_mode = S_IFCHR;
    status->st_blksize = 0;
    return 0;
}

int _isatty(int file)
{
    return file == STDIN_FILENO || file == STDOUT_FILENO ||
           file == STDERR_FILENO;
}
