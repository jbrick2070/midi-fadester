/*
 * Tubester - report listener test over a real loopback socket (REQUIREMENTS R-LNK-2, R-DCK-6)
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifdef NDEBUG
#undef NDEBUG
#endif

#include "tubester/listener.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <util/platform.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct deck_spy {
	int calls;
	char last_event[32];
	long long last_seq;
};

static void on_report(void *deck, obs_data_t *report, uint64_t now_ns)
{
	struct deck_spy *s = deck;
	assert(now_ns > 0);
	s->calls++;
	snprintf(s->last_event, sizeof(s->last_event), "%s", obs_data_get_string(report, "ev"));
	s->last_seq = obs_data_get_int(report, "seq");
}

/* Sends raw bytes to the listener and returns the status code of the reply, or 0 */
static int send_raw(int port, const char *request, size_t len)
{
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	assert(s != INVALID_SOCKET);
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((unsigned short)port)};
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	DWORD timeout = 3000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
	size_t sent = 0;
	while (sent < len) {
		int n = send(s, request + sent, (int)(len - sent), 0);
		assert(n > 0);
		sent += (size_t)n;
	}
	shutdown(s, SD_SEND);
	char reply[512] = {0};
	size_t have = 0;
	for (;;) {
		int n = recv(s, reply + have, (int)(sizeof(reply) - 1 - have), 0);
		if (n <= 0)
			break;
		have += (size_t)n;
		if (have >= sizeof(reply) - 1)
			break;
	}
	closesocket(s);
	int code = 0;
	if (sscanf(reply, "HTTP/1.1 %d", &code) != 1)
		return 0;
	return code;
}

/* A string literal sent without its terminator: no byte count typed by hand */
#define send_lit(port, s) send_raw((port), (s), sizeof(s) - 1)

#define SEC 1000000000ULL

static SOCKET connect_to(int port)
{
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	assert(s != INVALID_SOCKET);
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((unsigned short)port)};
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	return s;
}

/* True once the server has answered or closed: the socket is readable */
static bool answered(SOCKET s)
{
	fd_set set;
	FD_ZERO(&set);
	FD_SET(s, &set);
	struct timeval zero = {0, 0};
	return select(0, &set, NULL, NULL, &zero) > 0;
}

static int post(int port, const char *body)
{
	char req[4096];
	int n = snprintf(
		req, sizeof(req),
		"POST /report HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n\r\n%s",
		strlen(body), body);
	assert(n > 0 && (size_t)n < sizeof(req));
	return send_raw(port, req, (size_t)n);
}

int main(void)
{
	printf("test-tubester-listener\n");
	WSADATA wsa;
	assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);

	assert(tub_listener_port() == 0);
	struct deck_spy a = {0}, b = {0};
	assert(!tub_listener_register("", on_report, &a));
	assert(!tub_listener_register("deck-a", NULL, &a));
	assert(tub_listener_register("deck-a", on_report, &a));
	assert(!tub_listener_register("deck-a", on_report, &b)); /* an id is taken once */
	assert(!tub_listener_register("deck-b", on_report, &a)); /* and a deck registers once */
	assert(tub_listener_register("deck-b", on_report, &b));
	int port = tub_listener_port();
	assert(port > 0 && port < 65536);
	printf("  listening on port %d\n", port);

	/* a report reaches its deck and no other (R-DCK-6) */
	assert(post(port, "{\"deck\":\"deck-a\",\"inc\":\"p1\",\"seq\":1,\"ev\":\"tick\"}") == 204);
	assert(a.calls == 1 && b.calls == 0 && strcmp(a.last_event, "tick") == 0 && a.last_seq == 1);
	assert(post(port, "{\"deck\":\"deck-b\",\"inc\":\"p2\",\"seq\":7,\"ev\":\"hello\"}") == 204);
	assert(a.calls == 1 && b.calls == 1 && strcmp(b.last_event, "hello") == 0 && b.last_seq == 7);

	/* an unknown deck, a body that is not a report, and a body with no deck are dropped, with the same reply */
	assert(post(port, "{\"deck\":\"deck-z\",\"inc\":\"p\",\"seq\":1,\"ev\":\"tick\"}") == 204);
	assert(post(port, "not json at all") == 204);
	assert(post(port, "[1,2,3]") == 204);
	assert(post(port, "{\"inc\":\"p\",\"seq\":1,\"ev\":\"tick\"}") == 204);
	assert(post(port, "") == 204);
	assert(a.calls == 1 && b.calls == 1);

	/* other requests get nothing */
	assert(send_lit(port, "GET /report HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n") == 404);
	assert(send_lit(port, "POST /report HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n") == 404); /* no length */
	assert(send_lit(port, "POST /report HTTP/1.1\r\nContent-Length: 70000\r\n\r\n") == 404);
	assert(send_lit(port, "garbage\r\n") == 400); /* headers never complete; the client closed */
	{
		/* a valid report to any other path is refused before it is read */
		const char *other = "POST /other HTTP/1.1\r\nContent-Length: 48\r\n\r\n"
				    "{\"deck\":\"deck-a\",\"inc\":\"p1\",\"seq\":5,\"ev\":\"tick\"}";
		assert(send_raw(port, other, strlen(other)) == 404);
		const char *query = "POST /report?x=1 HTTP/1.1\r\nContent-Length: 48\r\n\r\n"
				    "{\"deck\":\"deck-a\",\"inc\":\"p1\",\"seq\":5,\"ev\":\"tick\"}";
		assert(send_raw(port, query, strlen(query)) == 404);
	}
	assert(a.calls == 1 && b.calls == 1);

	/* the port is held exclusively: not even a socket with SO_REUSEADDR can bind it */
	{
		SOCKET thief = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		BOOL on = TRUE;
		assert(setsockopt(thief, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on)) == 0);
		struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((unsigned short)port)};
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		assert(bind(thief, (struct sockaddr *)&addr, sizeof(addr)) != 0);
		closesocket(thief);
	}

	/* a client that trickles bytes and never finishes its headers is cut off at the deadline, and the next report
	 * goes through at once (the decks' reports are never held for long) */
	{
		/* timed from before the connect, so the server's whole 1.5 s window is inside the measurement; a busy runner
		 * can only make it longer, which the upper bound allows for */
		uint64_t t0 = os_gettime_ns();
		SOCKET slow = connect_to(port);
		const char *head = "POST /report HTTP/1.1\r\n";
		assert(send(slow, head, (int)strlen(head), 0) == (int)strlen(head));
		bool done = false;
		while (!done && os_gettime_ns() - t0 < 8 * SEC) {
			os_sleep_ms(250);
			send(slow, "X", 1, 0); /* fails once the server has closed; that is fine */
			done = answered(slow);
		}
		double secs = (double)(os_gettime_ns() - t0) / (double)SEC;
		closesocket(slow);
		printf("  a trickling client was cut off after %.2f s\n", secs);
		assert(done && secs >= 1.4 && secs < 4.0);
		uint64_t t1 = os_gettime_ns();
		assert(post(port, "{\"deck\":\"deck-b\",\"inc\":\"p2\",\"seq\":9,\"ev\":\"tick\"}") == 204);
		assert(os_gettime_ns() - t1 < SEC);
		assert(b.calls == 2 && b.last_seq == 9);
	}

	/* a body that never completes is refused at the deadline and reaches no deck */
	{
		uint64_t t0 = os_gettime_ns();
		SOCKET slow = connect_to(port);
		const char *partial = "POST /report HTTP/1.1\r\nContent-Length: 49\r\n\r\n{\"deck\":\"deck-a\"";
		assert(send(slow, partial, (int)strlen(partial), 0) == (int)strlen(partial));
		while (!answered(slow) && os_gettime_ns() - t0 < 8 * SEC)
			os_sleep_ms(50);
		double secs = (double)(os_gettime_ns() - t0) / (double)SEC;
		char reply[64] = {0};
		recv(slow, reply, sizeof(reply) - 1, 0);
		closesocket(slow);
		assert(secs >= 1.4 && secs < 4.0);
		assert(strncmp(reply, "HTTP/1.1 400", 12) == 0);
		assert(a.calls == 1);
	}

	/* a body split across two sends arrives whole */
	{
		const char *body = "{\"deck\":\"deck-a\",\"inc\":\"p1\",\"seq\":2,\"ev\":\"playing\"}";
		char head[256];
		int n = snprintf(head, sizeof(head), "POST /report HTTP/1.1\r\nContent-Length: %zu\r\n\r\n",
				 strlen(body));
		SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((unsigned short)port)};
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		assert(connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0);
		assert(send(s, head, n, 0) == n);
		assert(send(s, body, 10, 0) == 10);
		os_sleep_ms(50);
		assert(send(s, body + 10, (int)strlen(body) - 10, 0) == (int)strlen(body) - 10);
		char reply[64] = {0};
		recv(s, reply, sizeof(reply) - 1, 0);
		closesocket(s);
		assert(strncmp(reply, "HTTP/1.1 204", 12) == 0);
		assert(a.calls == 2 && strcmp(a.last_event, "playing") == 0 && a.last_seq == 2);
	}

	/* after unregister, nothing reaches the deck; the other deck still hears */
	tub_listener_unregister(&a);
	assert(post(port, "{\"deck\":\"deck-a\",\"inc\":\"p1\",\"seq\":3,\"ev\":\"tick\"}") == 204);
	assert(a.calls == 2);
	assert(post(port, "{\"deck\":\"deck-b\",\"inc\":\"p2\",\"seq\":10,\"ev\":\"tick\"}") == 204);
	assert(b.calls == 3);
	tub_listener_unregister(&a); /* twice is harmless */
	tub_listener_unregister(NULL);

	/* the id is free again */
	struct deck_spy a2 = {0};
	assert(tub_listener_register("deck-a", on_report, &a2));
	assert(post(port, "{\"deck\":\"deck-a\",\"inc\":\"p9\",\"seq\":1,\"ev\":\"tick\"}") == 204);
	assert(a2.calls == 1 && a.calls == 2);

	/* stop closes the port; a second stop is harmless; a register afterwards starts it again on some port */
	tub_listener_stop();
	assert(tub_listener_port() == 0);
	tub_listener_stop();
	{
		SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((unsigned short)port)};
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		assert(connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0);
		closesocket(s);
	}
	struct deck_spy c = {0};
	assert(tub_listener_register("deck-c", on_report, &c));
	int port2 = tub_listener_port();
	assert(port2 > 0);
	assert(post(port2, "{\"deck\":\"deck-c\",\"inc\":\"p\",\"seq\":1,\"ev\":\"tick\"}") == 204);
	assert(c.calls == 1 && b.calls == 3); /* deck-b was forgotten by stop */

	/* a stop in the middle of a slow request returns at once instead of waiting for the client */
	{
		SOCKET slow = connect_to(port2);
		const char *head = "POST /report HTTP/1.1\r\n";
		assert(send(slow, head, (int)strlen(head), 0) == (int)strlen(head));
		os_sleep_ms(150); /* the listener is now inside that request */
		uint64_t t0 = os_gettime_ns();
		tub_listener_stop();
		double secs = (double)(os_gettime_ns() - t0) / (double)SEC;
		closesocket(slow);
		printf("  stop with a slow client connected took %.3f s\n", secs);
		assert(secs < 1.0);
		assert(tub_listener_port() == 0);
	}
	/* unregistering after stop is harmless (a deck destroyed after module unload does this) */
	tub_listener_unregister(&c);

	WSACleanup();
	printf("test-tubester-listener: ALL PASS\n");
	return 0;
}
