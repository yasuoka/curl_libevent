/*
 * Copyright (c) 2025 YASUOKA Masahiko <yasuoka@yasuoka.net>
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
#include <unistd.h>
#include <stdlib.h>
#include <event.h>
#include <paths.h>
#include <err.h>

#include <curl/curl.h>
#include "curl_libevent.h"

static void curl_on_done(void *, CURLMsg *);

static int	ncurl = 0;

int
main(int argc, char *argv[])
{
	int			 i, ch;
	struct curl_libevent	*evcurl;
	CURL 			*curl;
	FILE			*fdevnull;
	struct event_base	*eb;

	while ((ch = getopt(argc, argv, "")) != -1)
		switch (ch) {
		}
	argc -= optind;
	argv += optind;

	if ((fdevnull = fopen(_PATH_DEVNULL, "a+")) == NULL)
		err(1, "fopen(%s)", _PATH_DEVNULL);

	eb = event_init();
	curl_global_init(CURL_GLOBAL_DEFAULT);
	evcurl = curl_libevent_create(eb);

	for (i = 0; i < argc; i++) {
		curl = curl_easy_init();
		curl_easy_setopt(curl, CURLOPT_URL, argv[i]);
		curl_easy_setopt(curl, CURLOPT_PRIVATE, argv[i]);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, fdevnull);
		curl_libevent_perform(evcurl, curl, curl_on_done);
		ncurl++;
	}

	if (ncurl > 0)
		event_loop(0);

	curl_libevent_destroy(evcurl);
	event_loop(0);	/* make sure no event is scheduled */

	curl_global_cleanup();

	exit(EXIT_SUCCESS);
}

void
curl_on_done(void *ctx, CURLMsg *msg)
{
	long	 rescode;
	if (msg->data.result == CURLE_OK) {
		curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &rescode);
		printf("OK %-30.30s %03ld\n", (char *)ctx, rescode);
	} else
		printf("NG %s \n", (char *)ctx);
	if (--ncurl <= 0)
		event_loopbreak();

	curl_easy_cleanup(msg->easy_handle);
}
