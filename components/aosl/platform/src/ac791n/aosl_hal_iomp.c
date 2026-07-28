#define BOOL_DEFINE_CONFLICT
#include <hal/aosl_hal_iomp.h>
#include <hal/aosl_hal_errno.h>
#include <lwip/sockets.h>
#include <sys/errno.h>
#include <hal/aosl_hal_memory.h>

// Epoll stubs
int aosl_hal_epoll_create() { return -1; }
int aosl_hal_epoll_destroy(int epfd) { return 0; }
int aosl_hal_epoll_ctl(int epfd, aosl_epoll_op_e op, int fd, aosl_poll_event_t *ev) { return -1; }
int aosl_hal_epoll_wait(int epfd, aosl_poll_event_t *evlist, int maxevents, int timeout_ms) { return -1; }
int aosl_hal_poll(aosl_poll_event_t fds[], int nfds, int timeout_ms) { return -1; }

// Select / FDSet
fd_set_t aosl_hal_fdset_create()
{
	fd_set *fds = aosl_hal_malloc(sizeof(fd_set));
	return fds;
}

void aosl_hal_fdset_destroy(fd_set_t fdset)
{
	if (fdset) aosl_hal_free(fdset);
}

void aosl_hal_fdset_zero(fd_set_t fdset)
{
	FD_ZERO((fd_set *)fdset);
}

void aosl_hal_fdset_set(fd_set_t fdset, int fd)
{
	FD_SET(fd, (fd_set *)fdset);
}

void aosl_hal_fdset_clr(fd_set_t fdset, int fd)
{
	FD_CLR(fd, (fd_set *)fdset);
}

int aosl_hal_fdset_isset(fd_set_t fdset, int fd)
{
	return FD_ISSET(fd, (fd_set *)fdset);
}

int aosl_hal_select(int nfds, fd_set_t readfds, fd_set_t writefds, fd_set_t exceptfds, int timeout_ms)
{
	int err;
	struct timeval tv, *ptv;
	if (timeout_ms < 0) {
		ptv = NULL;
	} else {
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		ptv = &tv;
	}

	err = select(nfds, (fd_set *)readfds, (fd_set *)writefds, (fd_set *)exceptfds, ptv);
	if (err < 0) {
		return aosl_hal_errno_convert(errno);
	}
	return err;
}
