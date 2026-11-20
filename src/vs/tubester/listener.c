/*
 * Tubester - the loopback listener that receives the deck pages' reports
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "tubester/listener.h"
#include "tubester/deck-link.h"
#include "tubester/http-parse.h"

#include "tubester/socket-compat.h"

#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

#include <stdio.h>
#include <string.h>

/* The blog of the host, so this file does not depend on the plugin-support target: tests link it alone. */
#include <util/base.h>
#define LISTENER_LOG(level, ...) blog(level, "[visual-sidechain] tape deck " __VA_ARGS__)

#define TUB_DECK_ID_MAX 64
#define TUB_DECKS_MAX 64
#define TUB_HEADERS_MAX 8192
/* A whole request, headers and body, must arrive within this time; a page posts a few hundred bytes at once. */
#define TUB_CONNECTION_DEADLINE_NS 1500000000ULL
/* A connection that has sent no byte at all by this time is closed (E-02). A page sends its request with the connect,
 * so only a client that is not a deck page waits in silence; a row of such clients, each holding this one thread for
 * the full 1.5 s, would keep every deck's reports out for longer than the 3 s link timeout. */
#define TUB_FIRST_BYTE_DEADLINE_NS 150000000ULL
/* Longest single wait, so a stop is seen within this time even in the middle of a request. */
#define TUB_POLL_US 200000

struct deck_entry {
	char id[TUB_DECK_ID_MAX + 1];
	tub_report_fn fn;
	void *deck;
};

/* Initialised statically (w32-pthreads sets it up on first use, under its own lock), so decks created on any thread
 * share it safely, and never destroyed: libobs unloads modules before it destroys the sources left at shutdown, so a
 * deck may unregister after tub_listener_stop. */
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct {
	bool wsa_ready; /* Windows only: WSAStartup has run */
	bool running;
	bool stopping; /* between tub_listener_stop's first and last lock: no new start */
	bool start_failure_logged;
	volatile bool stop;
	pthread_t thread;
	tub_socket_t sock;
	int port;
	struct deck_entry decks[TUB_DECKS_MAX];
	size_t deck_count;
} g = {.sock = TUB_INVALID_SOCKET};

static void respond(tub_socket_t client, const char *status_line)
{
	char reply[160];
	int n = snprintf(reply, sizeof(reply), "%s\r\nConnection: close\r\nContent-Length: 0\r\n\r\n", status_line);
	if (n > 0)
		tub_send(client, reply, (size_t)n);
	shutdown(client, TUB_SHUT_WR);
}

static void dispatch(const char *body, size_t len)
{
	obs_data_t *data = NULL;
	if (!tub_link_parse_body(body, len, &data))
		return;
	const char *deck_id = obs_data_get_string(data, "deck");
	uint64_t now = os_gettime_ns();
	pthread_mutex_lock(&g_mutex);
	for (size_t i = 0; deck_id && i < g.deck_count; i++) {
		if (strcmp(g.decks[i].id, deck_id) == 0) {
			g.decks[i].fn(g.decks[i].deck, data, now);
			break;
		}
	}
	pthread_mutex_unlock(&g_mutex);
	obs_data_release(data);
}

/* Reads up to want bytes into dst. Returns the count, 0 when the peer closed, or -1 on an error, at the deadline, or
 * when the listener is stopping. No single wait is longer than TUB_POLL_US, so neither a slow client nor a stop can
 * hold the thread past the deadline. */
static int recv_by(tub_socket_t client, char *dst, size_t want, uint64_t deadline_ns)
{
	for (;;) {
		if (os_atomic_load_bool(&g.stop))
			return -1;
		uint64_t now = os_gettime_ns();
		if (now >= deadline_ns)
			return -1;
		uint64_t wait_us = (deadline_ns - now) / 1000;
		if (wait_us > TUB_POLL_US)
			wait_us = TUB_POLL_US;
		int ready = tub_socket_wait_readable(client, wait_us);
		if (ready < 0)
			return -1;
		if (ready == 0)
			continue;
		int n = tub_recv(client, dst, want);
		return n < 0 ? -1 : n;
	}
}

/* One request per connection: read the headers, then exactly Content-Length bytes, answer, close. */
static void handle_connection(tub_socket_t client)
{
	tub_socket_set_timeout(client, SO_SNDTIMEO, 1000);
	const uint64_t start = os_gettime_ns();
	uint64_t deadline = start + TUB_CONNECTION_DEADLINE_NS;

	size_t cap = TUB_HEADERS_MAX + TUB_HTTP_BODY_MAX;
	char *buf = bmalloc(cap);
	size_t have = 0, headers_len = 0;
	while (have < TUB_HEADERS_MAX) {
		int n = recv_by(client, buf + have, TUB_HEADERS_MAX - have,
				have == 0 ? start + TUB_FIRST_BYTE_DEADLINE_NS : deadline);
		if (n <= 0)
			break;
		have += (size_t)n;
		headers_len = tub_http_headers_end(buf, have);
		if (headers_len)
			break;
	}
	if (os_atomic_load_bool(&g.stop)) {
		bfree(buf);
		return;
	}
	if (!headers_len) {
		respond(client, "HTTP/1.1 400 Bad Request");
		bfree(buf);
		return;
	}
	size_t body_len = 0;
	if (!tub_http_is_report_post(buf, headers_len) || !tub_http_content_length(buf, headers_len, &body_len)) {
		respond(client, "HTTP/1.1 404 Not Found");
		bfree(buf);
		return;
	}
	size_t total = headers_len + body_len;
	while (have < total) {
		int n = recv_by(client, buf + have, total - have, deadline);
		if (n <= 0)
			break;
		have += (size_t)n;
	}
	if (os_atomic_load_bool(&g.stop)) {
		bfree(buf);
		return;
	}
	if (have < total) {
		respond(client, "HTTP/1.1 400 Bad Request");
		bfree(buf);
		return;
	}
	dispatch(buf + headers_len, body_len);
	respond(client, "HTTP/1.1 204 No Content");
	bfree(buf);
}

static void *listener_thread(void *param)
{
	UNUSED_PARAMETER(param);
	os_set_thread_name("tubester-listener");
	bool error_logged = false;
	while (!os_atomic_load_bool(&g.stop)) {
		int ready = tub_socket_wait_readable(g.sock, TUB_POLL_US);
		if (ready == 0)
			continue;
		struct sockaddr_in from;
		tub_socklen_t from_len = sizeof(from);
		tub_socket_t client = ready > 0 ? accept(g.sock, (struct sockaddr *)&from, &from_len)
						: TUB_INVALID_SOCKET;
		if (client == TUB_INVALID_SOCKET) {
			/* A failed wait returns at once, and a failed accept (no descriptor left) leaves the connection
			 * queued, so the wait says readable again: pause, or this thread spins a core (E-03). */
			if (!error_logged) {
				error_logged = true;
				LISTENER_LOG(LOG_WARNING, "listener: %s failed (%d); retrying",
					     ready > 0 ? "accept" : "wait", tub_socket_error());
			}
			os_sleep_ms(50);
			continue;
		}
		error_logged = false;
		tub_socket_no_inherit(client);
		/* Bound to 127.0.0.1, so every peer is local; the check costs nothing and documents the intent. */
		if (from.sin_addr.s_addr == htonl(INADDR_LOOPBACK))
			handle_connection(client);
		tub_socket_close(client);
	}
	return NULL;
}

/* A failure is logged once until the listener starts again, so decks that retry do not flood the log. */
static void log_start_failure(const char *what, int code)
{
	if (g.start_failure_logged)
		return;
	g.start_failure_logged = true;
	LISTENER_LOG(LOG_ERROR, "listener: %s failed (%d); decks retry", what, code);
}

static bool start_locked(void)
{
	if (g.running)
		return true;
	if (g.stopping)
		return false;
#ifdef _WIN32
	if (!g.wsa_ready) {
		WSADATA wsa;
		int err = WSAStartup(MAKEWORD(2, 2), &wsa);
		if (err != 0) {
			log_start_failure("WSAStartup", err);
			return false;
		}
		g.wsa_ready = true;
	}
	/* Not inheritable, so no process OBS starts holds a copy of the port; overlapped like socket(), so the send
	 * timeout still applies */
	tub_socket_t s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
				    WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
	if (s == TUB_INVALID_SOCKET) {
		log_start_failure("socket", tub_socket_error());
		return false;
	}
	/* No other socket, even one of the same user with SO_REUSEADDR, may bind this port while it is ours */
	BOOL exclusive = TRUE;
	if (setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof(exclusive)) != 0) {
		log_start_failure("SO_EXCLUSIVEADDRUSE", tub_socket_error());
		tub_socket_close(s);
		return false;
	}
#else
	tub_socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == TUB_INVALID_SOCKET) {
		log_start_failure("socket", tub_socket_error());
		return false;
	}
	/* Close-on-exec, so no process OBS starts holds a copy of the port. Neither SO_REUSEADDR nor SO_REUSEPORT is set:
	 * on POSIX that alone keeps every other socket, even one of the same user with SO_REUSEADDR, from binding this
	 * address and port while it is ours (the hold Windows gets from SO_EXCLUSIVEADDRUSE). */
	tub_socket_no_inherit(s);
#endif
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = 0};
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(s, 16) != 0) {
		log_start_failure("bind or listen", tub_socket_error());
		tub_socket_close(s);
		return false;
	}
	struct sockaddr_in bound;
	tub_socklen_t bound_len = sizeof(bound);
	if (getsockname(s, (struct sockaddr *)&bound, &bound_len) != 0) {
		log_start_failure("getsockname", tub_socket_error());
		tub_socket_close(s);
		return false;
	}
	g.sock = s;
	g.port = ntohs(bound.sin_port);
	os_atomic_set_bool(&g.stop, false);
	if (pthread_create(&g.thread, NULL, listener_thread, NULL) != 0) {
		log_start_failure("thread start", 0);
		tub_socket_close(s);
		g.sock = TUB_INVALID_SOCKET;
		g.port = 0;
		return false;
	}
	g.running = true;
	g.start_failure_logged = false;
	LISTENER_LOG(LOG_INFO, "listener: reports accepted on 127.0.0.1 (port chosen by the system)");
	return true;
}

bool tub_listener_register(const char *deck_id, tub_report_fn fn, void *deck)
{
	if (!deck_id || !*deck_id || strlen(deck_id) > TUB_DECK_ID_MAX || !fn || !deck)
		return false;
	pthread_mutex_lock(&g_mutex);
	bool ok = start_locked();
	if (ok) {
		if (g.deck_count >= TUB_DECKS_MAX) {
			ok = false;
		} else {
			for (size_t i = 0; i < g.deck_count; i++) {
				if (strcmp(g.decks[i].id, deck_id) == 0 || g.decks[i].deck == deck)
					ok = false;
			}
		}
	}
	if (ok) {
		struct deck_entry *e = &g.decks[g.deck_count++];
		snprintf(e->id, sizeof(e->id), "%s", deck_id);
		e->fn = fn;
		e->deck = deck;
	}
	pthread_mutex_unlock(&g_mutex);
	return ok;
}

void tub_listener_unregister(void *deck)
{
	if (!deck)
		return;
	pthread_mutex_lock(&g_mutex);
	for (size_t i = 0; i < g.deck_count; i++) {
		if (g.decks[i].deck == deck) {
			g.decks[i] = g.decks[g.deck_count - 1];
			memset(&g.decks[g.deck_count - 1], 0, sizeof(g.decks[0]));
			g.deck_count--;
			break;
		}
	}
	pthread_mutex_unlock(&g_mutex);
}

int tub_listener_port(void)
{
	pthread_mutex_lock(&g_mutex);
	int port = g.running ? g.port : 0;
	pthread_mutex_unlock(&g_mutex);
	return port;
}

void tub_listener_stop(void)
{
	pthread_mutex_lock(&g_mutex);
	if (g.stopping) {
		pthread_mutex_unlock(&g_mutex);
		return;
	}
	bool was_running = g.running;
	g.running = false;
	g.stopping = true;
	g.deck_count = 0;
	memset(g.decks, 0, sizeof(g.decks));
	/* The thread sees the flag within TUB_POLL_US, even in the middle of a request, and skips dispatch once it is
	 * set; with no decks left, a dispatch already under way calls nobody. */
	os_atomic_set_bool(&g.stop, true);
	pthread_mutex_unlock(&g_mutex);
	if (was_running) {
		pthread_join(g.thread, NULL);
		tub_socket_close(g.sock);
	}
	pthread_mutex_lock(&g_mutex);
	g.sock = TUB_INVALID_SOCKET;
	g.port = 0;
#ifdef _WIN32
	if (g.wsa_ready) {
		WSACleanup();
		g.wsa_ready = false;
	}
#endif
	g.stopping = false;
	pthread_mutex_unlock(&g_mutex);
}
