/* Minimal iconv for devkitPPC newlib, which declares iconv but ships no implementation.
 *
 * NetSurf only uses iconv for edge cases (form submission charsets, local filenames);
 * page decoding goes through libparserutils' own codecs. Supported: UTF-8, US-ASCII,
 * ISO-8859-1, WINDOWS-1252, UTF-16{,BE,LE}, UCS-2, UCS-4/UTF-32{,BE,LE}. A "//TRANSLIT" or
 * "//IGNORE" suffix makes unencodable characters become '?' (or get dropped) instead of EILSEQ. */
#include <errno.h>
#include <iconv.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum enc { E_UTF8, E_ASCII, E_LATIN1, E_CP1252, E_UTF16BE, E_UTF16LE, E_UCS4BE, E_UCS4LE };
enum { F_TRANSLIT = 1, F_IGNORE = 2 };

struct cd {
	enum enc from, to;
	int flags;
};

/* Windows-1252 0x80..0x9F; 0 marks undefined bytes. */
static const uint16_t cp1252[32] = {
	0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
	0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

static int parse_enc(const char *name, enum enc *e, int *flags)
{
	char n[32];
	size_t len = strcspn(name, "/");
	if (len >= sizeof n)
		return -1;
	memcpy(n, name, len);
	n[len] = '\0';
	if (flags) {
		if (strstr(name, "//TRANSLIT"))
			*flags |= F_TRANSLIT;
		if (strstr(name, "//IGNORE"))
			*flags |= F_IGNORE;
	}
	static const struct {
		const char *name;
		enum enc e;
	} names[] = {
		{"UTF-8", E_UTF8}, {"UTF8", E_UTF8},
		{"US-ASCII", E_ASCII}, {"ASCII", E_ASCII}, {"ANSI_X3.4-1968", E_ASCII},
		{"ISO-8859-1", E_LATIN1}, {"ISO8859-1", E_LATIN1}, {"LATIN1", E_LATIN1},
		{"WINDOWS-1252", E_CP1252}, {"CP1252", E_CP1252},
		{"UTF-16", E_UTF16BE}, {"UTF-16BE", E_UTF16BE}, {"UCS-2", E_UTF16BE},
		{"UCS-2BE", E_UTF16BE}, {"UTF-16LE", E_UTF16LE}, {"UCS-2LE", E_UTF16LE},
		{"UCS-4", E_UCS4BE}, {"UCS-4BE", E_UCS4BE}, {"UTF-32", E_UCS4BE},
		{"UTF-32BE", E_UCS4BE}, {"UCS-4LE", E_UCS4LE}, {"UTF-32LE", E_UCS4LE},
	};
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
		if (strcasecmp(n, names[i].name) == 0) {
			*e = names[i].e;
			return 0;
		}
	}
	return -1;
}

iconv_t iconv_open(const char *to, const char *from)
{
	struct cd *cd = calloc(1, sizeof *cd);
	if (!cd) {
		errno = ENOMEM;
		return (iconv_t)-1;
	}
	if (parse_enc(to, &cd->to, &cd->flags) != 0 || parse_enc(from, &cd->from, NULL) != 0) {
		free(cd);
		errno = EINVAL;
		return (iconv_t)-1;
	}
	return (iconv_t)cd;
}

int iconv_close(iconv_t cd)
{
	free(cd);
	return 0;
}

/* Decodes one code point. Returns bytes consumed, 0 if input is incomplete, -1 if invalid. */
static int decode(enum enc e, const unsigned char *s, size_t n, uint32_t *cp)
{
	switch (e) {
	case E_ASCII:
		if (s[0] > 0x7F)
			return -1;
		/* fall through */
	case E_LATIN1:
		*cp = s[0];
		return 1;
	case E_CP1252:
		if (s[0] >= 0x80 && s[0] <= 0x9F) {
			*cp = cp1252[s[0] - 0x80];
			return *cp ? 1 : -1;
		}
		*cp = s[0];
		return 1;
	case E_UTF8: {
		unsigned c = s[0];
		int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : -1;
		if (len < 0)
			return -1;
		if ((size_t)len > n)
			return 0;
		uint32_t v = len == 1 ? c : c & (0x3F >> (len - 1));
		for (int i = 1; i < len; i++) {
			if ((s[i] & 0xC0) != 0x80)
				return -1;
			v = (v << 6) | (s[i] & 0x3F);
		}
		*cp = v;
		return len;
	}
	case E_UTF16BE:
	case E_UTF16LE: {
		if (n < 2)
			return 0;
		uint32_t u = e == E_UTF16BE ? (s[0] << 8 | s[1]) : (s[1] << 8 | s[0]);
		if (u >= 0xD800 && u < 0xDC00) {
			if (n < 4)
				return 0;
			uint32_t l = e == E_UTF16BE ? (s[2] << 8 | s[3]) : (s[3] << 8 | s[2]);
			if (l < 0xDC00 || l > 0xDFFF)
				return -1;
			*cp = 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00);
			return 4;
		}
		*cp = u;
		return 2;
	}
	case E_UCS4BE:
	case E_UCS4LE:
		if (n < 4)
			return 0;
		*cp = e == E_UCS4BE ? ((uint32_t)s[0] << 24 | s[1] << 16 | s[2] << 8 | s[3])
				    : ((uint32_t)s[3] << 24 | s[2] << 16 | s[1] << 8 | s[0]);
		return 4;
	}
	return -1;
}

/* Encodes one code point into out. Returns bytes written, 0 if out is too small,
 * -1 if the code point can't be represented. */
static int encode(enum enc e, uint32_t cp, unsigned char *o, size_t n)
{
	switch (e) {
	case E_ASCII:
		if (cp > 0x7F)
			return -1;
		/* fall through */
	case E_LATIN1:
		if (cp > 0xFF)
			return -1;
		if (n < 1)
			return 0;
		o[0] = (unsigned char)cp;
		return 1;
	case E_CP1252:
		if (n < 1)
			return 0;
		if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) {
			o[0] = (unsigned char)cp;
			return 1;
		}
		for (int i = 0; i < 32; i++) {
			if (cp1252[i] == cp && cp) {
				o[0] = (unsigned char)(0x80 + i);
				return 1;
			}
		}
		return -1;
	case E_UTF8: {
		int len = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
		if ((size_t)len > n)
			return 0;
		if (len == 1) {
			o[0] = (unsigned char)cp;
		} else {
			for (int i = len - 1; i > 0; i--) {
				o[i] = (unsigned char)(0x80 | (cp & 0x3F));
				cp >>= 6;
			}
			o[0] = (unsigned char)((0xF00 >> len) | cp);
		}
		return len;
	}
	case E_UTF16BE:
	case E_UTF16LE: {
		uint16_t u[2];
		int units = 1;
		if (cp >= 0x10000) {
			cp -= 0x10000;
			u[0] = (uint16_t)(0xD800 + (cp >> 10));
			u[1] = (uint16_t)(0xDC00 + (cp & 0x3FF));
			units = 2;
		} else {
			u[0] = (uint16_t)cp;
		}
		if (n < (size_t)units * 2)
			return 0;
		for (int i = 0; i < units; i++) {
			o[i * 2 + (e == E_UTF16LE)] = (unsigned char)(u[i] >> 8);
			o[i * 2 + (e == E_UTF16BE)] = (unsigned char)u[i];
		}
		return units * 2;
	}
	case E_UCS4BE:
	case E_UCS4LE:
		if (n < 4)
			return 0;
		for (int i = 0; i < 4; i++)
			o[e == E_UCS4BE ? 3 - i : i] = (unsigned char)(cp >> (8 * i));
		return 4;
	}
	return -1;
}

size_t iconv(iconv_t cdp, char **__restrict inbuf, size_t *__restrict inleft,
	char **__restrict outbuf, size_t *__restrict outleft)
{
	struct cd *cd = (struct cd *)cdp;
	size_t irreversible = 0;
	if (!inbuf || !*inbuf)
		return 0; /* stateless: nothing to reset or flush */
	while (*inleft > 0) {
		uint32_t cp;
		int in = decode(cd->from, (const unsigned char *)*inbuf, *inleft, &cp);
		if (in == 0) {
			errno = EINVAL;
			return (size_t)-1;
		}
		if (in < 0) {
			errno = EILSEQ;
			return (size_t)-1;
		}
		int out = encode(cd->to, cp, (unsigned char *)*outbuf, *outleft);
		int dropped = 0;
		if (out < 0) {
			if (cd->flags & F_IGNORE) {
				out = 0;
				dropped = 1;
			} else if (cd->flags & F_TRANSLIT) {
				out = encode(cd->to, '?', (unsigned char *)*outbuf, *outleft);
			} else {
				errno = EILSEQ;
				return (size_t)-1;
			}
			irreversible++;
		}
		if (out == 0 && !dropped) {
			errno = E2BIG;
			return (size_t)-1;
		}
		*inbuf += in;
		*inleft -= (size_t)in;
		*outbuf += out;
		*outleft -= (size_t)out;
	}
	return irreversible;
}
