/* Brings networking up off the UI thread: DHCP can take several seconds on the Wii. */
#include "app/app.h"
#include "core/platform.h"
#include "net/http.h"
#include "net/sock.h"
#include "net/tls.h"

static volatile int g_state = NET_DOWN;
static iac_thread g_thread;

static void *boot(void *arg)
{
	(void)arg;
	g_state = NET_STARTING;
	if (sock_init() != 0) {
		WL_LOG_E("NET", "network init failed (no connection configured?)");
		g_state = NET_FAILED;
		return NULL;
	}
	int rc = tls_global_init(APP_DIR "/cacert.pem", APP_DIR "/seed.bin", TLS_FLAGS_DEFAULT);
	if (rc != 0) {
		WL_LOG_E("NET", "TLS init failed (-0x%04x)", (unsigned)-rc);
		g_state = NET_FAILED;
		return NULL;
	}
	if (http_init() != 0) {
		WL_LOG_E("NET", "HTTP init failed");
		g_state = NET_FAILED;
		return NULL;
	}
	WL_LOG_I("NET", "network ready");
	g_state = NET_READY;
	return NULL;
}

void net_boot_start(void)
{
	if (g_state != NET_DOWN)
		return;
	g_state = NET_STARTING;
	if (plat_thread_create(&g_thread, boot, NULL, 64 * 1024, 60) != 0)
		g_state = NET_FAILED;
}

int net_state(void)
{
	return g_state;
}
