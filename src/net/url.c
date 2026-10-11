#include "net/url.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int url_parse(const char *s, struct url *out)
{
	memset(out, 0, sizeof *out);
	const char *p;
	if (strncasecmp(s, "https://", 8) == 0) {
		out->https = 1;
		out->port = 443;
		p = s + 8;
	} else if (strncasecmp(s, "http://", 7) == 0) {
		out->port = 80;
		p = s + 7;
	} else {
		return -1;
	}
	size_t hl = strcspn(p, ":/?#");
	if (hl == 0 || hl >= URL_HOST_MAX || memchr(p, '@', hl))
		return -1;
	memcpy(out->host, p, hl);
	p += hl;
	if (*p == ':') {
		char *end;
		long port = strtol(p + 1, &end, 10);
		if (end == p + 1 || port <= 0 || port > 65535)
			return -1;
		out->port = (uint16_t)port;
		p = end;
	}
	size_t pl = strcspn(p, "#");
	if (pl == 0) {
		strcpy(out->path, "/");
	} else {
		if (pl + 2 > URL_PATH_MAX)
			return -1;
		if (*p == '?') { /* "host?query" */
			out->path[0] = '/';
			memcpy(out->path + 1, p, pl);
		} else if (*p == '/') {
			memcpy(out->path, p, pl);
		} else {
			return -1;
		}
	}
	return 0;
}

int url_resolve(const struct url *base, const char *loc, struct url *out)
{
	if (strncasecmp(loc, "http://", 7) == 0 || strncasecmp(loc, "https://", 8) == 0)
		return url_parse(loc, out);
	char buf[URL_HOST_MAX + URL_PATH_MAX + 16];
	if (loc[0] == '/' && loc[1] == '/') {
		snprintf(buf, sizeof buf, "%s:%s", base->https ? "https" : "http", loc);
		return url_parse(buf, out);
	}
	*out = *base;
	if (loc[0] == '/') {
		if (strlen(loc) >= URL_PATH_MAX)
			return -1;
		snprintf(out->path, sizeof out->path, "%s", loc);
		return 0;
	}
	/* Relative to the base path's directory. */
	const char *q = strchr(base->path, '?');
	size_t plen = q ? (size_t)(q - base->path) : strlen(base->path);
	const char *slash = base->path;
	for (size_t i = 0; i < plen; i++)
		if (base->path[i] == '/')
			slash = base->path + i;
	size_t dir = (size_t)(slash - base->path) + 1;
	if (dir + strlen(loc) >= URL_PATH_MAX)
		return -1;
	memcpy(out->path, base->path, dir);
	strcpy(out->path + dir, loc);
	return 0;
}

int url_encode(const char *s, char *out, size_t cap)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t n = 0;
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		int keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
			c == '-' || c == '_' || c == '.' || c == '~';
		size_t need = keep ? 1 : 3;
		if (n + need + 1 > cap)
			return -1;
		if (keep) {
			out[n++] = (char)c;
		} else {
			out[n++] = '%';
			out[n++] = hex[c >> 4];
			out[n++] = hex[c & 15];
		}
	}
	out[n] = '\0';
	return (int)n;
}
