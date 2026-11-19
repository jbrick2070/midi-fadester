/*
 * Tubester - the few socket calls that differ between Winsock and POSIX, for the report listener and its test
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

typedef SOCKET tub_socket_t;
typedef int tub_socklen_t;
#define TUB_INVALID_SOCKET INVALID_SOCKET
#define TUB_SHUT_WR SD_SEND

static inline int tub_socket_error(void)
{
	return WSAGetLastError();
}

static inline void tub_socket_close(tub_socket_t s)
{
	closesocket(s);
}

/* Winsock takes the timeout as a DWORD of milliseconds */
static inline int tub_socket_set_timeout(tub_socket_t s, int option, unsigned int ms)
{
	DWORD value = ms;
	return setsockopt(s, SOL_SOCKET, option, (const char *)&value, sizeof(value));
}

/* Keeps a socket out of every process OBS starts */
static inline void tub_socket_no_inherit(tub_socket_t s)
{
	SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
}

/* 1 when s is readable (or the peer closed) within timeout_us, 0 at the timeout, -1 on an error */
static inline int tub_socket_wait_readable(tub_socket_t s, uint64_t timeout_us)
{
	fd_set set;
	FD_ZERO(&set);
	FD_SET(s, &set);
	struct timeval wait = {.tv_sec = (long)(timeout_us / 1000000), .tv_usec = (long)(timeout_us % 1000000)};
	int ready = select(0, &set, NULL, NULL, &wait);
	return ready < 0 ? -1 : (ready > 0 ? 1 : 0);
}

#else /* POSIX: macOS (and any other Unix) */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

typedef int tub_socket_t;
typedef socklen_t tub_socklen_t;
#define TUB_INVALID_SOCKET (-1)
#define TUB_SHUT_WR SHUT_WR

static inline int tub_socket_error(void)
{
	return errno;
}

static inline void tub_socket_close(tub_socket_t s)
{
	close(s);
}

/* POSIX takes the timeout as a struct timeval */
static inline int tub_socket_set_timeout(tub_socket_t s, int option, unsigned int ms)
{
	struct timeval value = {.tv_sec = (time_t)(ms / 1000), .tv_usec = (suseconds_t)((ms % 1000) * 1000)};
	return setsockopt(s, SOL_SOCKET, option, &value, sizeof(value));
}

/* Keeps a socket out of every process OBS starts, and stops a send to a peer that has gone from raising SIGPIPE,
 * which would end OBS: the send fails with EPIPE instead. */
static inline void tub_socket_no_inherit(tub_socket_t s)
{
	int flags = fcntl(s, F_GETFD);
	if (flags >= 0)
		fcntl(s, F_SETFD, flags | FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
	int on = 1;
	setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
}

/* 1 when s is readable (or the peer closed) within timeout_us, 0 at the timeout or on a signal, -1 on an error.
 * poll, not select: select cannot watch a descriptor numbered FD_SETSIZE or above, and OBS can hold that many. */
static inline int tub_socket_wait_readable(tub_socket_t s, uint64_t timeout_us)
{
	struct pollfd pfd = {.fd = s, .events = POLLIN};
	int ready = poll(&pfd, 1, (int)((timeout_us + 999) / 1000));
	if (ready < 0)
		return errno == EINTR ? 0 : -1;
	return ready > 0 ? 1 : 0;
}

#endif

/* MSG_NOSIGNAL where it exists (Linux); macOS uses SO_NOSIGPIPE, set by tub_socket_no_inherit */
#if defined(MSG_NOSIGNAL)
#define TUB_SEND_FLAGS MSG_NOSIGNAL
#else
#define TUB_SEND_FLAGS 0
#endif

/* recv and send with the length type each system wants; the count, or a negative number on an error */
static inline int tub_recv(tub_socket_t s, char *dst, size_t len)
{
#ifdef _WIN32
	return recv(s, dst, (int)len, 0);
#else
	return (int)recv(s, dst, len, 0);
#endif
}

static inline int tub_send(tub_socket_t s, const char *src, size_t len)
{
#ifdef _WIN32
	return send(s, src, (int)len, 0);
#else
	return (int)send(s, src, len, TUB_SEND_FLAGS);
#endif
}
