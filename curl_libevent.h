/*
 * Copyright (c) 2019, 2025 YASUOKA Masahiko <yasuoka@yasuoka.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#ifndef CURL_LIBEVENT_H
#define CURL_LIBEVENT_H

#include <stdbool.h>
#include <event.h>
#include <curl/curl.h>

#ifdef __cplusplus
extern "C" {
#endif
struct curl_libevent;
struct curl_libevent
	*curl_libevent_create(struct event_base *);
CURLM	*curl_libevent_handle(struct curl_libevent *);
void	 curl_libevent_set_auto_proxy_config(struct curl_libevent *, bool);

void	 curl_libevent_perform(struct curl_libevent *, CURL *,
	    void (*on_done)(void *, CURLMsg *));
void	 curl_libevent_destroy(struct curl_libevent *);
#ifdef __cplusplus
}
#endif
#endif
