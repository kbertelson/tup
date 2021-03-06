#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "dir_mutex.h"

#ifdef _WIN32
#define mkdir(a,b) mkdir(a)
#endif

int mkdirat(int dirfd, const char *pathname, mode_t mode)
{
	int rc;

	if(mode) {/* for win32 */}

	dir_mutex_lock(dirfd);
	rc = mkdir(pathname, mode);
	dir_mutex_unlock();
	return rc;
}
