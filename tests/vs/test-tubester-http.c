/*
 * Tubester - HTTP request parsing and random token tests (REQUIREMENTS R-LNK-2)
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

#include "tubester/http-parse.h"
#include "tubester/random.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define REQ(s) s, strlen(s)

static void test_headers_end(void)
{
	assert(tub_http_headers_end(REQ("POST /report HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}")) == 44);
	assert(tub_http_headers_end(REQ("POST /report HTTP/1.1\r\nContent-Length: 2\r\n\r\n")) == 44);
	assert(tub_http_headers_end(REQ("POST /report HTTP/1.1\r\nContent-Length: 2\r\n")) == 0);
	assert(tub_http_headers_end(REQ("POST /report HTTP/1.1\r\nContent-Length: 2\r\n\r")) == 0);
	assert(tub_http_headers_end(REQ("")) == 0);
	assert(tub_http_headers_end(REQ("\r\n\r\n")) == 4);
	assert(tub_http_headers_end(NULL, 10) == 0);
	/* a bare LF pair does not end the headers; a browser always sends CRLF */
	assert(tub_http_headers_end(REQ("POST / HTTP/1.1\nContent-Length: 2\n\n")) == 0);
	printf("  test_headers_end: PASS\n");
}

static void test_is_report_post(void)
{
	assert(tub_http_is_report_post(REQ("POST /report HTTP/1.1\r\n\r\n")));
	assert(tub_http_is_report_post(REQ("POST /report HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n")));
	/* only that path, method and version */
	assert(!tub_http_is_report_post(REQ("POST / HTTP/1.1\r\n\r\n")));
	assert(!tub_http_is_report_post(REQ("POST /reportx HTTP/1.1\r\n\r\n")));
	assert(!tub_http_is_report_post(REQ("POST /report?x=1 HTTP/1.1\r\n\r\n")));
	assert(!tub_http_is_report_post(REQ("POST /other HTTP/1.1\r\n\r\n")));
	assert(!tub_http_is_report_post(REQ("GET /report HTTP/1.1\r\n\r\n")));
	assert(!tub_http_is_report_post(REQ("OPTIONS /report HTTP/1.1\r\n\r\n")));
	assert(!tub_http_is_report_post(REQ("POST /report HTTP/2.0\r\n\r\n")));
	assert(!tub_http_is_report_post(REQ("POST /report\r\n\r\n")));
	assert(!tub_http_is_report_post(REQ("post /report HTTP/1.1\r\n\r\n")));
	assert(!tub_http_is_report_post(REQ("POST /report HTTP/1.1 \r\n\r\n")));
	/* the request line must be complete within the headers given */
	assert(!tub_http_is_report_post("POST /report HTTP/1.1\r\n", 20));
	assert(!tub_http_is_report_post(REQ("")));
	assert(!tub_http_is_report_post(NULL, 30));
	printf("  test_is_report_post: PASS\n");
}

static void test_content_length(void)
{
	size_t n = 999;
	assert(tub_http_content_length(REQ("POST / HTTP/1.1\r\nContent-Length: 42\r\n\r\n"), &n) && n == 42);
	assert(tub_http_content_length(REQ("POST / HTTP/1.1\r\ncontent-length:0\r\n\r\n"), &n) && n == 0);
	assert(tub_http_content_length(REQ("POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nCONTENT-LENGTH:  7 \r\nX: y\r\n\r\n"),
				       &n) &&
	       n == 7);
	assert(tub_http_content_length(REQ("POST / HTTP/1.1\r\nContent-Length: 65536\r\n\r\n"), &n) && n == 65536);
	/* too large, missing, repeated, not a number, negative, empty */
	assert(!tub_http_content_length(REQ("POST / HTTP/1.1\r\nContent-Length: 65537\r\n\r\n"), &n));
	assert(!tub_http_content_length(REQ("POST / HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n\r\n"), &n));
	assert(!tub_http_content_length(REQ("POST / HTTP/1.1\r\nHost: x\r\n\r\n"), &n));
	assert(!tub_http_content_length(REQ("POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n"), &n));
	assert(!tub_http_content_length(REQ("POST / HTTP/1.1\r\nContent-Length: 1a\r\n\r\n"), &n));
	assert(!tub_http_content_length(REQ("POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n"), &n));
	assert(!tub_http_content_length(REQ("POST / HTTP/1.1\r\nContent-Length:\r\n\r\n"), &n));
	assert(!tub_http_content_length(REQ("POST / HTTP/1.1\r\nContent-Length: 1 2\r\n\r\n"), &n));
	/* a Content-Length in the request line is not a header */
	assert(!tub_http_content_length(REQ("POST /Content-Length:5 HTTP/1.1\r\n\r\n"), &n));
	assert(!tub_http_content_length(NULL, 20, &n));
	assert(!tub_http_content_length(REQ("POST / HTTP/1.1\r\nContent-Length: 1\r\n\r\n"), NULL));
	printf("  test_content_length: PASS\n");
}

static void test_random(void)
{
	char a[33], b[33];
	assert(tub_random_hex(a, 32) && strlen(a) == 32);
	assert(tub_random_hex(b, 32) && strlen(b) == 32);
	assert(strcmp(a, b) != 0);
	for (size_t i = 0; i < 32; i++)
		assert((a[i] >= '0' && a[i] <= '9') || (a[i] >= 'a' && a[i] <= 'f'));
	char odd[8] = "xxxxxxx";
	assert(tub_random_hex(odd, 7) && strlen(odd) == 7);
	char none[2] = "x";
	assert(tub_random_hex(none, 0) && none[0] == '\0');
	assert(!tub_random_hex(NULL, 4));
	printf("  test_random: PASS\n");
}

int main(void)
{
	printf("test-tubester-http\n");
	test_headers_end();
	test_is_report_post();
	test_content_length();
	test_random();
	printf("test-tubester-http: ALL PASS\n");
	return 0;
}
