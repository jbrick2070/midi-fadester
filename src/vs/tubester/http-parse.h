/*
 * Tubester - the few parts of an HTTP request the report listener reads
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

#ifdef __cplusplus
extern "C" {
#endif

/* The most a report body may hold; tub_link_parse_body refuses more as well (R-LNK-2). */
#define TUB_HTTP_BODY_MAX 65536

/* The offset just past the blank line that ends the headers, or 0 while the headers are not complete. */
size_t tub_http_headers_end(const char *buf, size_t len);

/* True when the request line is exactly "POST /report HTTP/1.1" or "POST /report HTTP/1.0". Reports go only there;
 * any other method, path or query is refused. */
bool tub_http_is_report_post(const char *buf, size_t headers_len);

/* True with *out set when the headers carry one Content-Length of 0 to TUB_HTTP_BODY_MAX. The header name is matched
 * without regard to case. A missing, repeated, negative or too large value gives false. */
bool tub_http_content_length(const char *buf, size_t headers_len, size_t *out);

#ifdef __cplusplus
}
#endif
