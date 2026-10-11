/* TCP socket shim: libogc net_* on the Wii, BSD sockets on the host. IPv4 only. */
#ifndef IAC_SOCK_H
#define IAC_SOCK_H

#include <stddef.h>
#include <stdint.h>

enum {
	SOCK_ERR_IO = -1001,
	SOCK_ERR_TIMEOUT = -1002,
	SOCK_ERR_DNS = -1003,
	SOCK_ERR_CONNECT = -1004,
	SOCK_ERR_INIT = -1005,
	SOCK_WOULD_BLOCK = -1006,
};

/* Non-blocking API for the HTTP event loop (one network thread drives every socket). */
enum { SOCK_POLL_IN = 1, SOCK_POLL_OUT = 2, SOCK_POLL_ERR = 4 };
/* Non-blocking DNS: 0 with *addr_be set, SOCK_WOULD_BLOCK while a resolver thread looks it up,
 * or SOCK_ERR_DNS (failures are cached for 30 s). */
int sock_resolve_async(const char *host, uint32_t *addr_be);
/* Creates a non-blocking TCP socket and starts connecting. Returns fd or SOCK_ERR_*. */
int sock_nb_open(uint32_t addr_be, uint16_t port);
/* Continues a connect: 1 = connected, 0 = still in progress, <0 = SOCK_ERR_CONNECT. */
int sock_nb_connect_poll(int fd, uint32_t addr_be, uint16_t port);
/* Return bytes moved (>0), 0 on orderly close (recv only), SOCK_WOULD_BLOCK, or SOCK_ERR_IO. */
int sock_nb_send(int fd, const void *buf, size_t len);
int sock_nb_recv(int fd, void *buf, size_t len);
/* Waits up to timeout_ms for any of n sockets; events/revents use SOCK_POLL_*.
 * Returns the number ready (0 on timeout) or SOCK_ERR_IO. */
int sock_poll_many(const int *fds, const int *events, int *revents, int n, int timeout_ms);

/* Brings up the network stack (DHCP on the Wii). Safe to call more than once. */
int sock_init(void);
/* SO_RCVBUF applied to sockets created afterwards (0 = leave the stack default). */
void sock_set_rcvbuf(int bytes);
/* Resolves host to an IPv4 address in network byte order. Results are cached. */
int sock_resolve(const char *host, uint32_t *addr_be);
/* Returns a connected socket descriptor or a SOCK_ERR_* code. */
int sock_connect(const char *host, uint16_t port, int timeout_ms);
/* Sends the whole buffer. Returns len or a SOCK_ERR_* code. */
int sock_send_all(int fd, const void *buf, size_t len, int timeout_ms);
/* Returns bytes read (>0), 0 on orderly close, or a SOCK_ERR_* code. */
int sock_recv(int fd, void *buf, size_t len, int timeout_ms);
void sock_close(int fd);
const char *sock_strerror(int err);

#endif
