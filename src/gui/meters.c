/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * meters.c - peak levels of the UA-1000's audio, for the desk's meters
 *
 * The device sends no levels on its control port (measured on the device),
 * so they are read from the audio itself, through the
 * sound server that holds the card: a JACK client.  On PipeWire, JACK
 * clients run through pipewire-jack; with jackd they run natively.  libjack
 * is loaded at run time (dlopen): without it, or without a server, there
 * are no meters and nothing else changes.  With the card held by one
 * program directly through ALSA, no other program can read it: no meters.
 *
 * Sources: the card's capture channels (WAVE IN 1-10, after the device's
 * WAVE IN patch bay) and, on PipeWire, the monitor of its playback channels
 * (WAVE OUT 1-10, what the computer sends).  The JACK process callback only
 * keeps the highest absolute sample of each port since the GUI last read it.
 */
#include "meters.h"
#include "ua1000_midi.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The parts of the JACK API used here, declared rather than included, so
 * the build needs no JACK headers.  These are JACK's stable C ABI.
 */
typedef struct jack_client jack_client_t;
typedef struct jack_port jack_port_t;
typedef uint32_t jack_nframes_t;
typedef int (*jack_process_cb)(jack_nframes_t nframes, void *arg);
enum { JACK_NO_START_SERVER = 0x01 };
enum { JACK_PORT_IS_INPUT = 0x1, JACK_PORT_IS_OUTPUT = 0x2 };
#define JACK_AUDIO_TYPE "32 bit float mono audio"

struct jack_api {
	void *lib;
	jack_client_t *(*client_open)(const char *name, int options,
				      int *status, ...);
	int (*client_close)(jack_client_t *client);
	jack_port_t *(*port_register)(jack_client_t *client, const char *name,
				      const char *type, unsigned long flags,
				      unsigned long buffer_size);
	const char **(*get_ports)(jack_client_t *client, const char *name,
				  const char *type, unsigned long flags);
	void (*free)(void *ptr);
	int (*connect)(jack_client_t *client, const char *src, const char *dst);
	const char *(*port_name)(const jack_port_t *port);
	int (*port_connected)(const jack_port_t *port);
	void *(*port_get_buffer)(jack_port_t *port, jack_nframes_t frames);
	int (*set_process_callback)(jack_client_t *client, jack_process_cb cb,
				    void *arg);
	int (*activate)(jack_client_t *client);
	int (*deactivate)(jack_client_t *client);
	void (*on_shutdown)(jack_client_t *client, void (*cb)(void *arg),
			    void *arg);
	/* optional: a port's aliases (absent: not checked) */
	jack_port_t *(*port_by_name)(jack_client_t *client, const char *name);
	int (*port_get_aliases)(const jack_port_t *port,
				char *const aliases[2]);
	/* optional: libjack's messages, silenced (absent: left as they are) */
	void (*set_error_function)(void (*fn)(const char *msg));
	void (*set_info_function)(void (*fn)(const char *msg));
};

struct meters {
	struct jack_api jack;
	jack_client_t *client;
	jack_port_t *port[METERS_MAX_SOURCES];
	enum meter_source_kind kind[METERS_MAX_SOURCES];
	unsigned int channel[METERS_MAX_SOURCES]; /* 0-based on the card */
	unsigned int n;
	bool system_ok; /* jackd: the card's ports are "system:..." */
	/* set by the JACK thread when the server goes away */
	atomic_bool gone;
	/* the peak since the last read, as float bits (the audio thread
	 * raises it, the GUI takes it back to 0) */
	_Atomic uint32_t peak[METERS_MAX_SOURCES];
};

static bool load_api(struct jack_api *j)
{
	j->lib = dlopen("libjack.so.0", RTLD_NOW | RTLD_LOCAL);
	if (!j->lib)
		return false;
#define SYM(field, name)                                                       \
	do {                                                                   \
		*(void **)&j->field = dlsym(j->lib, name);                     \
		if (!j->field)                                                 \
			return false;                                          \
	} while (0)
	SYM(client_open, "jack_client_open");
	SYM(client_close, "jack_client_close");
	SYM(port_register, "jack_port_register");
	SYM(get_ports, "jack_get_ports");
	SYM(free, "jack_free");
	SYM(connect, "jack_connect");
	SYM(port_name, "jack_port_name");
	SYM(port_connected, "jack_port_connected");
	SYM(port_get_buffer, "jack_port_get_buffer");
	SYM(set_process_callback, "jack_set_process_callback");
	SYM(activate, "jack_activate");
	SYM(deactivate, "jack_deactivate");
	SYM(on_shutdown, "jack_on_shutdown");
#undef SYM
	*(void **)&j->port_by_name = dlsym(j->lib, "jack_port_by_name");
	*(void **)&j->port_get_aliases = dlsym(j->lib, "jack_port_get_aliases");
	*(void **)&j->set_error_function =
		dlsym(j->lib, "jack_set_error_function");
	*(void **)&j->set_info_function =
		dlsym(j->lib, "jack_set_info_function");
	return true;
}

static uint32_t float_bits(float f)
{
	uint32_t u;

	memcpy(&u, &f, sizeof(u));
	return u;
}

static float bits_float(uint32_t u)
{
	float f;

	memcpy(&f, &u, sizeof(f));
	return f;
}

/* The audio thread: no allocation, no locks; one compare-exchange per port
 * per period (the positive floats order as their bits do). */
static int process(jack_nframes_t frames, void *arg)
{
	struct meters *m = arg;

	for (unsigned int i = 0; i < m->n; i++) {
		const float *buf = m->jack.port_get_buffer(m->port[i], frames);
		float peak = 0.0f;
		uint32_t old, now;

		for (jack_nframes_t k = 0; k < frames; k++) {
			float a = fabsf(buf[k]);

			if (a > peak)
				peak = a;
		}
		now = float_bits(peak);
		old = atomic_load_explicit(&m->peak[i], memory_order_relaxed);
		while (now > old &&
		       !atomic_compare_exchange_weak_explicit(
			       &m->peak[i], &old, now, memory_order_relaxed,
			       memory_order_relaxed))
			;
	}
	return 0;
}

/*
 * A port of the UA-1000 and its 0-based channel: PipeWire names them
 * "<card>:capture_AUX<n>" and "<card>:monitor_AUX<n>" (n from 0); jackd
 * names its device's "system:capture_<n>" (n from 1).
 */
/* "<prefix><digits>" and nothing after: the index, in *n */
static bool port_index(const char *s, const char *prefix, unsigned int *n)
{
	size_t len = strlen(prefix);
	char *end;
	unsigned long v;

	if (strncmp(s, prefix, len) != 0 || s[len] < '0' || s[len] > '9')
		return false;
	errno = 0;
	v = strtoul(s + len, &end, 10);
	if (errno != 0 || *end != '\0' || v > UINT_MAX)
		return false;
	*n = (unsigned int)v;
	return true;
}

static bool classify(const char *name, bool system_ok,
		     enum meter_source_kind *kind, unsigned int *channel)
{
	const char *colon = strrchr(name, ':');
	unsigned int n;

	if (!colon)
		return false;
	if (strstr(name, "UA-1000")) {
		if (port_index(colon + 1, "capture_AUX", &n))
			*kind = METER_CAPTURE;
		else if (port_index(colon + 1, "monitor_AUX", &n))
			*kind = METER_PLAYBACK;
		else
			return false;
		*channel = n;
		return true;
	}
	if (system_ok && strncmp(name, "system:", 7) == 0 &&
	    port_index(colon + 1, "capture_", &n) && n >= 1) {
		*kind = METER_CAPTURE;
		*channel = n - 1;
		return true;
	}
	return false;
}

bool meters_alsa_alias_is_card(const char *alias, int index, const char *id)
{
	static const char prefix[] = "alsa_pcm:";
	const char *dev, *end, *colon = strrchr(alias, ':');
	char number[16];
	size_t len;

	if (strncmp(alias, prefix, sizeof(prefix) - 1) != 0 || !colon)
		return true; /* not an ALSA alias: nothing to say */
	dev = alias + sizeof(prefix) - 1; /* "hw:2:out1", "hw:UA1000,0:out1" */
	if (strncmp(dev, "hw:", 3) == 0)
		dev += 3;
	else if (strncmp(dev, "plughw:", 7) == 0)
		dev += 7;
	else
		return false; /* "default" and the like: not the UA-1000's */
	end = dev + strcspn(dev, ",:");
	len = (size_t)(end - dev);
	snprintf(number, sizeof(number), "%d", index);
	return (len == strlen(number) && strncmp(dev, number, len) == 0) ||
	       (id && *id && len == strlen(id) && strncmp(dev, id, len) == 0);
}

/*
 * jackd's ports are all "system:capture_<n>", whatever card it runs on;
 * jackd gives each the alias "alsa_pcm:<device>:out<n>" (measured: jackd
 * -d alsa -d hw:2 named them "alsa_pcm:hw:2:out1" ...).  A port whose ALSA
 * alias names another card is not the UA-1000's.
 */
static bool system_port_is_ua1000(struct meters *m, const char *name)
{
	char a0[320] = "", a1[320] = "";
	char *const aliases[2] = { a0, a1 };
	char id[64] = "";
	int index, n;
	jack_port_t *port;

	if (!m->jack.port_by_name || !m->jack.port_get_aliases)
		return true;
	port = m->jack.port_by_name(m->client, name);
	if (!port)
		return false;
	n = m->jack.port_get_aliases(port, aliases);
	if (n <= 0)
		return true;
	if (ua1000_midi_card(&index, id, sizeof(id)) != 0)
		return false;
	for (int i = 0; i < n && i < 2; i++)
		if (!meters_alsa_alias_is_card(aliases[i], index, id))
			return false;
	return true;
}

static void add_sources(struct meters *m, const char **names, bool system_ok)
{
	for (size_t i = 0; names && names[i] && m->n < METERS_MAX_SOURCES;
	     i++) {
		enum meter_source_kind kind;
		unsigned int ch;
		char port_name[32];
		jack_port_t *port;

		if (!classify(names[i], system_ok, &kind, &ch) ||
		    ch >= METERS_MAX_CHANNELS ||
		    (system_ok && !system_port_is_ua1000(m, names[i])))
			continue;
		snprintf(port_name, sizeof(port_name), "%s_%u",
			 kind == METER_CAPTURE ? "in" : "out", ch + 1);
		port = m->jack.port_register(m->client, port_name,
					     JACK_AUDIO_TYPE,
					     JACK_PORT_IS_INPUT, 0);
		if (!port)
			continue;
		m->port[m->n] = port;
		m->kind[m->n] = kind;
		m->channel[m->n] = ch;
		m->n++;
	}
}

/*
 * Connect each source to its meter port (after activation); with
 * only_idle, only the meter ports left with no connection: the device was
 * switched off or unplugged, and the server removed its ports and made
 * them again when it came back (measured on the device: after a power
 * cycle every meter stayed unconnected, dead until a restart).
 */
static void connect_sources(struct meters *m, bool only_idle)
{
	const char **names = m->jack.get_ports(m->client, NULL, JACK_AUDIO_TYPE,
					       JACK_PORT_IS_OUTPUT);

	for (size_t i = 0; names && names[i]; i++) {
		enum meter_source_kind kind;
		unsigned int ch;

		if (!classify(names[i], m->system_ok, &kind, &ch))
			continue;
		for (unsigned int k = 0; k < m->n; k++)
			if (m->kind[k] == kind && m->channel[k] == ch &&
			    !(only_idle &&
			      m->jack.port_connected(m->port[k]) > 0))
				m->jack.connect(m->client, names[i],
						m->jack.port_name(m->port[k]));
	}
	if (names)
		m->jack.free((void *)names);
}

void meters_reconnect(struct meters *m)
{
	bool idle = false;

	if (!m)
		return;
	for (unsigned int k = 0; k < m->n && !idle; k++)
		idle = m->jack.port_connected(m->port[k]) <= 0;
	if (idle)
		connect_sources(m, true);
}

/* libjack's own messages: a server that is not running is a normal state
 * here (the meters retry), not something to print every 2 s. */
static void quiet(const char *msg)
{
	(void)msg;
}

/* The JACK thread: the server went away (stopped, restarted). */
static void on_server_gone(void *arg)
{
	struct meters *m = arg;

	atomic_store(&m->gone, true);
}

bool meters_alive(struct meters *m)
{
	return m && !atomic_load(&m->gone);
}

struct meters *meters_open(void)
{
	struct meters *m = calloc(1, sizeof(*m));
	const char **names;
	bool found = false;

	if (!m)
		return NULL;
	if (!load_api(&m->jack))
		goto fail;
	if (m->jack.set_error_function)
		m->jack.set_error_function(quiet);
	if (m->jack.set_info_function)
		m->jack.set_info_function(quiet);
	m->client = m->jack.client_open("UA-1000 Sidecard meters",
					JACK_NO_START_SERVER, NULL);
	if (!m->client)
		goto fail;
	m->jack.on_shutdown(m->client, on_server_gone, m);
	names = m->jack.get_ports(m->client, "UA-1000", JACK_AUDIO_TYPE,
				  JACK_PORT_IS_OUTPUT);
	found = names && names[0];
	add_sources(m, names, false);
	if (names)
		m->jack.free((void *)names);
	if (!found) {
		/* jackd: its device's ports are "system:..." */
		names = m->jack.get_ports(m->client, "^system:capture_",
					  JACK_AUDIO_TYPE, JACK_PORT_IS_OUTPUT);
		add_sources(m, names, true);
		if (names)
			m->jack.free((void *)names);
	}
	if (m->n == 0 ||
	    m->jack.set_process_callback(m->client, process, m) != 0 ||
	    m->jack.activate(m->client) != 0)
		goto fail;
	m->system_ok = !found;
	connect_sources(m, false);
	return m;

fail:
	meters_close(m);
	return NULL;
}

void meters_close(struct meters *m)
{
	if (!m)
		return;
	if (m->client) {
		m->jack.deactivate(m->client);
		m->jack.client_close(m->client);
	}
	if (m->jack.lib)
		dlclose(m->jack.lib);
	free(m);
}

float meters_take_peak(struct meters *m, enum meter_source_kind kind,
		       unsigned int channel, bool *available)
{
	*available = false;
	if (!m)
		return 0.0f;
	for (unsigned int i = 0; i < m->n; i++) {
		if (m->kind[i] == kind && m->channel[i] == channel) {
			*available = true;
			return bits_float(atomic_exchange_explicit(
				&m->peak[i], 0, memory_order_relaxed));
		}
	}
	return 0.0f;
}
