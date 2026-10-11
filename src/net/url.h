/* Minimal URL handling for the HTTP client: http(s) only. */
#ifndef IAC_URL_H
#define IAC_URL_H

#include <stddef.h>
#include <stdint.h>

#define URL_HOST_MAX 128
#define URL_PATH_MAX 1536

struct url {
	int https;
	char host[URL_HOST_MAX];
	uint16_t port;
	char path[URL_PATH_MAX]; /* path + query, always starts with '/' */
};

/* 0 on success. Accepts http:// and https://, an optional :port, no userinfo. */
int url_parse(const char *s, struct url *out);
/* Resolves a Location header (absolute, scheme-relative "//host/..", or "/path") against base. */
int url_resolve(const struct url *base, const char *location, struct url *out);
/* Percent-encodes s for a query value (RFC 3986 unreserved kept, space -> %20). Returns length,
 * or -1 if it doesn't fit in cap (including the NUL). */
int url_encode(const char *s, char *out, size_t cap);

#endif
