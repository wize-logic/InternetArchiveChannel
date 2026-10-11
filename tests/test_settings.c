/* Host unit test: settings parse/format round trip and clamping. */
#include <stdio.h>
#include <string.h>

#include "core/settings.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
	struct settings s, t;
	settings_defaults(&s);
	CHECK(s.overscan == 5 && s.fast_h264 == 1 && s.debug_overlay == 0);

	settings_parse(&s, "# comment\r\n overscan = 9 \nfast_h264=0\nunknown=1\nlast_search=grateful dead  \n");
	CHECK(s.overscan == 9);
	CHECK(s.fast_h264 == 0);

	settings_parse(&s, "overscan=99\ndebug_overlay=1");
	CHECK(s.overscan == 15);
	CHECK(s.debug_overlay == 1);

	char buf[512];
	settings_format(&s, buf, sizeof buf);
	settings_defaults(&t);
	settings_parse(&t, buf);
	CHECK(memcmp(&s, &t, sizeof s) == 0);

	printf("%s\n", failures ? "FAILED" : "ok");
	return failures != 0;
}
