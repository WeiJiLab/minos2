#include <fcntl.h>
#include <stdarg.h>
#include "syscall.h"

#include <minos/kobject.h>

int open(const char *filename, int flags, ...)
{
	mode_t mode = 0;

	if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}

	int fd = __sys_open(filename, flags, mode);
	if (fd > 0) {
		kobject_open(fd);
		if (kobject_mmap(fd) == (void *)-1) {
			kobject_close(fd);
			return -ENOMEM;
		}
	}

	return __syscall_ret(fd);
}

weak_alias(open, open64);