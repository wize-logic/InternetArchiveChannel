#include "net/sock.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "core/log.h"
#include "core/platform.h"

#ifdef HW_RVL
#include <network.h>
#include <poll.h>
#define S_SOCKET(d, t, p) net_socket(d, t, p)
#define S_CONNECT net_connect
#define S_SEND(s, b, n) net_send(s, b, n, 0)
#define S_RECV(s, b, n) net_recv(s, b, n, 0)
#define S_CLOSE net_close
#define S_GETHOST net_gethostbyname
#define S_SETSOCKOPT net_setsockopt
typedef struct pollsd s_pollfd;
#ifndef SOL_SOCKET
#define SOL_SOCKET 0xffff /* BSD value, which IOS uses; libogc doesn't define it */
#endif
#define S_POLL(p, n, t) net_poll(p, n, t)
#define POLL_FD socket
#include <fcntl.h>
/* IOS's own non-blocking flag; libogc passes net_fcntl flags through unchanged. */
#define IOS_O_NONBLOCK 0x04
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define S_SOCKET(d, t, p) socket(d, t, p)
#define S_CONNECT connect
#define S_SEND(s, b, n) send(s, b, n, MSG_NOSIGNAL)
#define S_RECV(s, b, n) recv(s, b, n, 0)
#define S_CLOSE close
#define S_GETHOST gethostbyname
#define S_SETSOCKOPT setsockopt
typedef struct pollfd s_pollfd;
#define S_POLL(p, n, t) poll(p, n, t)
#define POLL_FD fd
#endif

/* IOS rejects very large socket transfers; keep each call well under its limit. */
#define SOCK_IO_CHUNK 16384

#define DNS_CACHE_SIZE 16
#define DNS_TTL_MS (10 * 60 * 1000)
#define DNS_FAIL_TTL_MS (30 * 1000)

enum { DNS_EMPTY = 0, DNS_PENDING, DNS_OK, DNS_FAILED };

struct dns_entry {
	char host[96];
	int state;
	uint32_t addr_be;
	uint64_t expires_ms;
};

static struct dns_entry g_dns[DNS_CACHE_SIZE];
static int g_dns_next;
static iac_mutex g_dns_lock;   /* the cache */
static iac_mutex g_ghbn_lock;  /* gethostbyname is not reentrant */
static iac_thread g_resolver;
static int g_resolver_started;
static int g_ready;
static int g_rcvbuf;

void sock_set_rcvbuf(int bytes)
{
	g_rcvbuf = bytes;
}

int sock_init(void)
{
	if (g_ready)
		return 0;
#ifdef HW_RVL
	char ip[16] = {0};
	s32 r = if_config(ip, NULL, NULL, true, 20);
	if (r < 0) {
		log_printf("net: if_config failed (%d)\n", (int)r);
		return SOCK_ERR_INIT;
	}
	log_printf("net: up, ip %s\n", ip);
#endif
	plat_mutex_init(&g_dns_lock);
	plat_mutex_init(&g_ghbn_lock);
	g_ready = 1;
	return 0;
}

static int lookup(const char *host, uint32_t *addr_be)
{
	plat_mutex_lock(&g_ghbn_lock);
	struct hostent *he = S_GETHOST(host);
	int ok = he && he->h_addrtype == AF_INET && he->h_addr_list && he->h_addr_list[0];
	if (ok)
		memcpy(addr_be, he->h_addr_list[0], 4);
	plat_mutex_unlock(&g_ghbn_lock);
	return ok ? 0 : SOCK_ERR_DNS;
}

/* Cache lookup under g_dns_lock; returns the live entry for host or NULL. */
static struct dns_entry *find_entry(const char *host, uint64_t now)
{
	for (int i = 0; i < DNS_CACHE_SIZE; i++) {
		struct dns_entry *e = &g_dns[i];
		if (e->state != DNS_EMPTY && strcmp(e->host, host) == 0) {
			if (e->state != DNS_PENDING && e->expires_ms <= now) {
				e->state = DNS_EMPTY;
				return NULL;
			}
			return e;
		}
	}
	return NULL;
}

static struct dns_entry *new_entry(const char *host)
{
	/* Prefer an empty slot; otherwise recycle round-robin, skipping pending lookups. */
	struct dns_entry *e = NULL;
	for (int i = 0; i < DNS_CACHE_SIZE && !e; i++)
		if (g_dns[i].state == DNS_EMPTY)
			e = &g_dns[i];
	for (int tries = 0; !e && tries < DNS_CACHE_SIZE; tries++) {
		struct dns_entry *c = &g_dns[g_dns_next];
		g_dns_next = (g_dns_next + 1) % DNS_CACHE_SIZE;
		if (c->state != DNS_PENDING)
			e = c;
	}
	if (!e)
		return NULL;
	memset(e, 0, sizeof *e);
	strncpy(e->host, host, sizeof e->host - 1);
	return e;
}

static void store_result(struct dns_entry *e, int rc, uint32_t addr, uint64_t now)
{
	e->state = rc == 0 ? DNS_OK : DNS_FAILED;
	e->addr_be = addr;
	e->expires_ms = now + (rc == 0 ? DNS_TTL_MS : DNS_FAIL_TTL_MS);
}

int sock_resolve(const char *host, uint32_t *addr_be)
{
	plat_mutex_lock(&g_dns_lock);
	struct dns_entry *e = find_entry(host, plat_ms());
	if (e && e->state == DNS_OK) {
		*addr_be = e->addr_be;
		plat_mutex_unlock(&g_dns_lock);
		return 0;
	}
	if (e && e->state == DNS_FAILED) {
		plat_mutex_unlock(&g_dns_lock);
		return SOCK_ERR_DNS;
	}
	plat_mutex_unlock(&g_dns_lock);

	uint32_t addr = 0;
	int rc = lookup(host, &addr);
	plat_mutex_lock(&g_dns_lock);
	e = find_entry(host, plat_ms());
	if (!e)
		e = new_entry(host);
	if (e && e->state != DNS_PENDING)
		store_result(e, rc, addr, plat_ms());
	plat_mutex_unlock(&g_dns_lock);
	if (rc == 0)
		*addr_be = addr;
	return rc;
}

/* Resolver thread: performs pending lookups one at a time so the HTTP event loop never blocks
 * in gethostbyname (an unknown host took 17 s to fail on the host OS). */
static void *resolver_thread(void *arg)
{
	(void)arg;
	for (;;) {
		char host[96] = "";
		plat_mutex_lock(&g_dns_lock);
		for (int i = 0; i < DNS_CACHE_SIZE && !host[0]; i++)
			if (g_dns[i].state == DNS_PENDING)
				memcpy(host, g_dns[i].host, sizeof host);
		plat_mutex_unlock(&g_dns_lock);
		if (!host[0]) {
			usleep(15000);
			continue;
		}
		uint32_t addr = 0;
		int rc = lookup(host, &addr);
		plat_mutex_lock(&g_dns_lock);
		for (int i = 0; i < DNS_CACHE_SIZE; i++)
			if (g_dns[i].state == DNS_PENDING && strcmp(g_dns[i].host, host) == 0)
				store_result(&g_dns[i], rc, addr, plat_ms());
		plat_mutex_unlock(&g_dns_lock);
		if (rc != 0)
			log_printf("net: DNS lookup for %s failed\n", host);
	}
	return NULL;
}

int sock_resolve_async(const char *host, uint32_t *addr_be)
{
	plat_mutex_lock(&g_dns_lock);
	if (!g_resolver_started) {
		g_resolver_started = plat_thread_create(&g_resolver, resolver_thread, NULL, 64 * 1024, 70) == 0;
		if (!g_resolver_started) {
			plat_mutex_unlock(&g_dns_lock);
			return sock_resolve(host, addr_be);
		}
	}
	int rc;
	struct dns_entry *e = find_entry(host, plat_ms());
	if (!e) {
		e = new_entry(host);
		if (e)
			e->state = DNS_PENDING;
		rc = e ? SOCK_WOULD_BLOCK : SOCK_ERR_DNS;
	} else if (e->state == DNS_OK) {
		*addr_be = e->addr_be;
		rc = 0;
	} else {
		rc = e->state == DNS_PENDING ? SOCK_WOULD_BLOCK : SOCK_ERR_DNS;
	}
	plat_mutex_unlock(&g_dns_lock);
	return rc;
}

/* A blocking connect can hang indefinitely (seen in Dolphin against slow archive.org storage
 * nodes), so connect non-blocking and bound it by timeout_ms. The socket is switched back to
 * blocking afterwards; send/recv bound their waits with poll. */
#ifdef HW_RVL
static int connect_with_timeout(int fd, struct sockaddr_in *sa, int timeout_ms)
{
	net_fcntl(fd, F_SETFL, IOS_O_NONBLOCK);
	uint64_t deadline = plat_ms() + (uint64_t)timeout_ms;
	int rc;
	for (;;) {
		/* IOS reports progress through repeated connect calls rather than POLLOUT. */
		rc = net_connect(fd, (struct sockaddr *)sa, sizeof *sa);
		if (rc >= 0 || rc == -EISCONN) {
			rc = 0;
			break;
		}
		if (rc != -EINPROGRESS && rc != -EALREADY) {
			rc = SOCK_ERR_CONNECT;
			break;
		}
		if (plat_ms() >= deadline) {
			rc = SOCK_ERR_TIMEOUT;
			break;
		}
		usleep(10000);
	}
	net_fcntl(fd, F_SETFL, 0);
	return rc;
}
#else
static int connect_with_timeout(int fd, struct sockaddr_in *sa, int timeout_ms)
{
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	int rc = connect(fd, (struct sockaddr *)sa, sizeof *sa);
	if (rc < 0 && errno == EINPROGRESS) {
		struct pollfd p = {.fd = fd, .events = POLLOUT};
		rc = poll(&p, 1, timeout_ms);
		if (rc == 0) {
			rc = SOCK_ERR_TIMEOUT;
		} else if (rc > 0) {
			int err = 0;
			socklen_t len = sizeof err;
			getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
			rc = err ? SOCK_ERR_CONNECT : 0;
		} else {
			rc = SOCK_ERR_CONNECT;
		}
	} else if (rc < 0) {
		rc = SOCK_ERR_CONNECT;
	}
	fcntl(fd, F_SETFL, flags);
	return rc;
}
#endif

int sock_connect(const char *host, uint16_t port, int timeout_ms)
{
	uint32_t addr;
	int rc = sock_resolve(host, &addr);
	if (rc < 0)
		return rc;

	int fd = S_SOCKET(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (fd < 0)
		return SOCK_ERR_IO;

	if (g_rcvbuf > 0) {
		/* Must precede connect so the advertised TCP window can use it. */
		int r = S_SETSOCKOPT(fd, SOL_SOCKET, SO_RCVBUF, &g_rcvbuf, sizeof g_rcvbuf);
		if (r < 0)
			log_printf("net: SO_RCVBUF %d rejected (%d)\n", g_rcvbuf, r);
	}
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	memcpy(&sa.sin_addr, &addr, 4);
#ifdef HW_RVL
	sa.sin_len = sizeof sa;
#endif
	rc = connect_with_timeout(fd, &sa, timeout_ms);
	if (rc < 0) {
		S_CLOSE(fd);
		return rc;
	}
	int one = 1;
	S_SETSOCKOPT(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	return fd;
}

static int wait_ready(int fd, int events, int timeout_ms)
{
	s_pollfd p;
	memset(&p, 0, sizeof p);
	p.POLL_FD = fd;
	p.events = events;
	int r = S_POLL(&p, 1, timeout_ms);
	if (r == 0)
		return SOCK_ERR_TIMEOUT;
	if (r < 0)
		return SOCK_ERR_IO;
	return 0;
}

int sock_send_all(int fd, const void *buf, size_t len, int timeout_ms)
{
	const unsigned char *p = buf;
	size_t left = len;
	while (left > 0) {
		int rc = wait_ready(fd, POLLOUT, timeout_ms);
		if (rc < 0)
			return rc;
		int n = S_SEND(fd, p, left > SOCK_IO_CHUNK ? SOCK_IO_CHUNK : (int)left);
		if (n <= 0) {
			log_printf("net: send on %d failed (%d)\n", fd, n);
			return SOCK_ERR_IO;
		}
		p += n;
		left -= (size_t)n;
	}
	return (int)len;
}

int sock_recv(int fd, void *buf, size_t len, int timeout_ms)
{
	int rc = wait_ready(fd, POLLIN, timeout_ms);
	if (rc < 0)
		return rc;
	int n = S_RECV(fd, buf, len > SOCK_IO_CHUNK ? SOCK_IO_CHUNK : (int)len);
	if (n < 0) {
#ifdef HW_RVL
		log_printf("net: recv on %d failed (%d)\n", fd, n); /* libogc returns -errno */
#else
		log_printf("net: recv on %d failed (errno %d)\n", fd, errno);
#endif
		return SOCK_ERR_IO;
	}
	return n;
}

void sock_close(int fd)
{
	if (fd >= 0)
		S_CLOSE(fd);
}

const char *sock_strerror(int err)
{
	switch (err) {
	case SOCK_ERR_IO: return "socket I/O error";
	case SOCK_ERR_TIMEOUT: return "timed out";
	case SOCK_ERR_DNS: return "DNS lookup failed";
	case SOCK_ERR_CONNECT: return "connection refused or unreachable";
	case SOCK_ERR_INIT: return "network init failed";
	default: return "unknown socket error";
	}
}

/* ---- non-blocking API -------------------------------------------------------------------- */

static void fill_addr(struct sockaddr_in *sa, uint32_t addr_be, uint16_t port)
{
	memset(sa, 0, sizeof *sa);
	sa->sin_family = AF_INET;
	sa->sin_port = htons(port);
	memcpy(&sa->sin_addr, &addr_be, 4);
#ifdef HW_RVL
	sa->sin_len = sizeof *sa;
#endif
}

static int set_nonblocking(int fd)
{
#ifdef HW_RVL
	return net_fcntl(fd, F_SETFL, IOS_O_NONBLOCK) < 0 ? -1 : 0;
#else
	int flags = fcntl(fd, F_GETFL, 0);
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

int sock_nb_open(uint32_t addr_be, uint16_t port)
{
	int fd = S_SOCKET(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (fd < 0)
		return SOCK_ERR_IO;
	if (set_nonblocking(fd) != 0) {
		S_CLOSE(fd);
		return SOCK_ERR_IO;
	}
	int one = 1;
	S_SETSOCKOPT(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	int rc = sock_nb_connect_poll(fd, addr_be, port);
	if (rc < 0) {
		S_CLOSE(fd);
		return rc;
	}
	return fd;
}

int sock_nb_connect_poll(int fd, uint32_t addr_be, uint16_t port)
{
	struct sockaddr_in sa;
	fill_addr(&sa, addr_be, port);
#ifdef HW_RVL
	/* IOS reports progress through repeated connect calls. */
	int rc = net_connect(fd, (struct sockaddr *)&sa, sizeof sa);
	if (rc >= 0 || rc == -EISCONN)
		return 1;
	if (rc == -EINPROGRESS || rc == -EALREADY)
		return 0;
	return SOCK_ERR_CONNECT;
#else
	int rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
	if (rc == 0 || errno == EISCONN)
		return 1;
	if (errno == EINPROGRESS || errno == EALREADY || errno == EAGAIN)
		return 0;
	return SOCK_ERR_CONNECT;
#endif
}

int sock_nb_send(int fd, const void *buf, size_t len)
{
	int n = S_SEND(fd, buf, len > SOCK_IO_CHUNK ? SOCK_IO_CHUNK : (int)len);
	if (n >= 0)
		return n;
#ifdef HW_RVL
	if (n == -EAGAIN || n == -EWOULDBLOCK)
		return SOCK_WOULD_BLOCK;
#else
	if (errno == EAGAIN || errno == EWOULDBLOCK)
		return SOCK_WOULD_BLOCK;
#endif
	return SOCK_ERR_IO;
}

int sock_nb_recv(int fd, void *buf, size_t len)
{
	int n = S_RECV(fd, buf, len > SOCK_IO_CHUNK ? SOCK_IO_CHUNK : (int)len);
	if (n >= 0)
		return n;
#ifdef HW_RVL
	if (n == -EAGAIN || n == -EWOULDBLOCK)
		return SOCK_WOULD_BLOCK;
#else
	if (errno == EAGAIN || errno == EWOULDBLOCK)
		return SOCK_WOULD_BLOCK;
#endif
	return SOCK_ERR_IO;
}

int sock_poll_many(const int *fds, const int *events, int *revents, int n, int timeout_ms)
{
	s_pollfd p[16];
	if (n > 16)
		n = 16;
	memset(p, 0, sizeof p);
	for (int i = 0; i < n; i++) {
		p[i].POLL_FD = fds[i];
		p[i].events = (events[i] & SOCK_POLL_IN ? POLLIN : 0) | (events[i] & SOCK_POLL_OUT ? POLLOUT : 0);
	}
	int r = S_POLL(p, n, timeout_ms);
	if (r < 0)
		return SOCK_ERR_IO;
	for (int i = 0; i < n; i++) {
		revents[i] = (p[i].revents & POLLIN ? SOCK_POLL_IN : 0) | (p[i].revents & POLLOUT ? SOCK_POLL_OUT : 0) |
			(p[i].revents & (POLLERR | POLLHUP | POLLNVAL) ? SOCK_POLL_ERR : 0);
	}
	return r;
}
