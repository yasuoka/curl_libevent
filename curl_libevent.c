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
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winhttp.h>
#endif

#include <sys/queue.h>

#ifndef _WIN32
#include <err.h>
#include <unistd.h>
#define xfree	free
#endif
#include <event.h>
#include <stdlib.h>
#include <stdbool.h>

#include <curl/curl.h>

#include "curl_libevent.h"

void		*xcalloc(size_t , size_t);
void		 xfree(void *);

/* curl glue */
static void	 curl_libevent_on_event(int, short, void *);
static void	 curl_libevent_on_timer(int, short, void *);
static void	 curl_libevent_events(struct curl_libevent *);
static int	 curl_libevent_set_events(CURL *, curl_socket_t, int, void *,
		    void *);
static int	 curl_libevent_set_timer(CURLM *, long , void *);

#ifdef _WIN32
static void	 curl_libevent_winhttp_callback(HINTERNET, DWORD_PTR, DWORD,
		    LPVOID, DWORD);
static void	 curl_libevent_on_pairs_event(intptr_t, short, void *);
static void	 freezero(void *, size_t);
static void	 warnx(const char *, ...);
static void	 vwarnx(const char *, va_list);
#endif

#ifdef CURL_LIBEVENT_DEBUG
#define CURL_LIBEVENT_DBG(arg)	warnx arg
#else
#define CURL_LIBEVENT_DBG(arg)	((void)0)
#endif

/************************************************************************
 * glue for libevent and curl
 ************************************************************************/
struct curl_libevent_sock;
struct curl_libevent_curl;

struct curl_libevent {
	CURLM			*handle;
	struct event		 ev_timer;
	struct event_base	*eb;
	bool			 autoproxy;
#ifdef _WIN32
	HANDLE			 hHttpSession;
	SOCKET			 pairs[2];
	struct event		 ev_pairs;
#endif
	TAILQ_HEAD(, curl_libevent_curl)
				 curls;
	TAILQ_HEAD(, curl_libevent_sock)
				 socks;
};

struct curl_libevent_sock {
	struct curl_libevent	*parent;
	int			 sock;
	struct event		 ev_sock;
	TAILQ_ENTRY(curl_libevent_sock)
				 next;
};

struct curl_libevent_curl {
	struct curl_libevent	 *parent;
	CURL			 *handle;
	void			(*on_done)(void *, CURLMsg *);
#ifdef _WIN32
	HANDLE			  hProxyResolv;
#endif
	TAILQ_ENTRY(curl_libevent_curl)
				  next;
};

#ifdef _WIN32
struct pair_event {
	DWORD_PTR	context;
	DWORD		dwError;
};
#endif

struct curl_libevent *
curl_libevent_create(struct event_base *eb)
{
	struct curl_libevent	*self;

	self = xcalloc(1, sizeof(*self));
	self->handle = curl_multi_init();
	self->eb = eb;
	TAILQ_INIT(&self->curls);
	TAILQ_INIT(&self->socks);

	curl_multi_setopt(self->handle, CURLMOPT_SOCKETFUNCTION,
	    curl_libevent_set_events);
	curl_multi_setopt(self->handle, CURLMOPT_TIMERFUNCTION,
	    curl_libevent_set_timer);
	curl_multi_setopt(self->handle, CURLMOPT_SOCKETDATA, self);
	curl_multi_setopt(self->handle, CURLMOPT_TIMERDATA, self);
	evtimer_set(&self->ev_timer, curl_libevent_on_timer, self);
	if (self->eb != NULL)
		event_base_set(self->eb, &self->ev_timer);

#ifdef _WIN32
	if ((self->hHttpSession = WinHttpOpen(L"curl_libevent",
	    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
	    WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC)) !=
	    INVALID_HANDLE_VALUE) {
		WinHttpSetStatusCallback(self->hHttpSession,
		    curl_libevent_winhttp_callback,
		    WINHTTP_CALLBACK_STATUS_GETPROXYFORURL_COMPLETE |
		    WINHTTP_CALLBACK_STATUS_REQUEST_ERROR, NULL);
	}
	self->pairs[0] = SOCKET_ERROR;
	self->pairs[1] = SOCKET_ERROR;
	if (evutil_socketpair(AF_UNIX, SOCK_STREAM, 0, self->pairs) != -1) {
		event_set(&self->ev_pairs, self->pairs[0],
		    EV_READ | EV_PERSIST, curl_libevent_on_pairs_event, self);
		if (self->eb != NULL)
			event_base_set(self->eb, &self->ev_pairs);
		event_add(&self->ev_pairs, NULL);
	}
#endif

	return (self);
}

void
curl_libevent_set_auto_proxy_config(struct curl_libevent *self, bool onoff)
{
	self->autoproxy = onoff;
}

void
curl_libevent_perform(struct curl_libevent *self, CURL *handle,
    void (*on_done)(void *, CURLMsg *))
{
	struct curl_libevent_curl *curl;


	curl = xcalloc(1, sizeof(*curl));
	curl->parent = self;
	curl->handle = handle;
	curl->on_done = on_done;

#ifdef _WIN32
	if (self->autoproxy) {
		char				*url;
#define URLMAXLEN	(128*1024)
		wchar_t				*urlw;
		int				 urllen;
		size_t				 urlwlen = 0;
		WINHTTP_CURRENT_USER_IE_PROXY_CONFIG
						 ieConfig;
		WINHTTP_AUTOPROXY_OPTIONS	 opts;
		DWORD				 dwError;

		curl->hProxyResolv = INVALID_HANDLE_VALUE;

		if (self->hHttpSession == INVALID_HANDLE_VALUE)
			goto skip;
		if (curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &url) !=
		    CURLE_OK)
			goto skip;
		if (!WinHttpGetIEProxyConfigForCurrentUser(&ieConfig))
			goto skip;
		if ((dwError = WinHttpCreateProxyResolver(self->hHttpSession,
		    &curl->hProxyResolv)) != ERROR_SUCCESS)
			goto skip;
		memset(&opts, 0, sizeof(opts));
		opts.dwFlags = WINHTTP_AUTOPROXY_ALLOW_AUTOCONFIG |
		    WINHTTP_AUTOPROXY_ALLOW_CM |
		    WINHTTP_AUTOPROXY_ALLOW_STATIC;
		if (ieConfig.fAutoDetect) {
			opts.dwFlags |= WINHTTP_AUTOPROXY_AUTO_DETECT;
			opts.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP
			    | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
			opts.lpszAutoConfigUrl = ieConfig.lpszAutoConfigUrl;
		}
		if (ieConfig.lpszProxy != NULL)
			opts.dwFlags |= WINHTTP_AUTOPROXY_ALLOW_STATIC;
		if ((urllen = strnlen_s(url, URLMAXLEN)) >= URLMAXLEN)
			goto skip;
		urlw = xcalloc(sizeof(wchar_t), urllen + 1);
		MultiByteToWideChar(CP_UTF8, 0,  url, -1, urlw, urllen + 1);

		if ((dwError = WinHttpGetProxyForUrlEx(curl->hProxyResolv,
		    urlw, &opts, curl)) != ERROR_IO_PENDING) {
			xfree(urlw);
			goto skip;
		}
		xfree(urlw);
		TAILQ_INSERT_TAIL(&self->curls, curl, next);
		return;
	}
#endif
 skip:
	curl_multi_add_handle(self->handle, handle);
	TAILQ_INSERT_TAIL(&self->curls, curl, next);
}

void
curl_libevent_on_event(int fd, short evmask, void *ctx)
{
	struct curl_libevent_sock
			*self = ctx;
	struct curl_libevent
			*parent;
	int		 flags = 0, running_handles = 0;

	if (evmask & EV_READ)
		flags |= CURL_CSELECT_IN;
	if (evmask & EV_WRITE)
		flags |= CURL_CSELECT_OUT;

	parent = self->parent;
	curl_multi_socket_action(self->parent->handle, self->sock, flags,
	    &running_handles);
	self = NULL;	/* self may be destroyed */
	curl_libevent_events(parent);
}

void
curl_libevent_on_timer(int fd, short evmask, void *ctx)
{
	struct curl_libevent	*self = ctx;
	int			 running_handles;

	curl_multi_socket_action(self->handle, CURL_SOCKET_TIMEOUT, 0,
	    &running_handles);

	curl_libevent_events(self);
}

void
curl_libevent_events(struct curl_libevent *self)
{
	int				 pending = 0;
	CURLMsg				*msg;
	void				*ctx = NULL;
	struct curl_libevent_curl	*curl;

	while ((msg = curl_multi_info_read(self->handle, &pending)) ) {
		switch (msg->msg) {
		case CURLMSG_DONE:
			curl_multi_remove_handle(self->handle,
			    msg->easy_handle);
			curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE,
			    &ctx);
			TAILQ_FOREACH(curl, &self->curls, next) {
				if (curl->handle == msg->easy_handle)
					break;
			}
			if (curl) {
				TAILQ_REMOVE(&self->curls, curl, next);
				if (curl->on_done)
					curl->on_done(ctx, msg);
				else
					curl_easy_cleanup(msg->easy_handle);
				freezero(curl, sizeof(*curl));
			} else {
				/* must not happen */
				warnx("Received a message for an "
				    "unknown curl easy handle");
				curl_easy_cleanup(msg->easy_handle);
			}
			break;
		case CURLMSG_NONE:
		case CURLMSG_LAST:
		default:
			warnx("%s: Received a %s message", __func__,
			    (msg->msg == CURLMSG_NONE)? "NONE" :
			    (msg->msg == CURLMSG_LAST)? "LAST" : "unknown");
			break;
		}
	}
}

void
curl_libevent_destroy(struct curl_libevent *self)
{
	struct curl_libevent_sock	*sock, *tsock;
	struct curl_libevent_curl	*curl, *tcurl;

	curl_multi_cleanup(self->handle);

	event_del(&self->ev_timer);
	TAILQ_FOREACH_SAFE(sock, &self->socks, next, tsock) {
		TAILQ_REMOVE(&self->socks, sock, next);
		event_del(&sock->ev_sock);
		freezero(sock, sizeof(*sock));
	}
	TAILQ_FOREACH_SAFE(curl, &self->curls, next, tcurl) {
		TAILQ_REMOVE(&self->curls, curl, next);
#ifdef _WIN32
		if (curl->hProxyResolv != INVALID_HANDLE_VALUE)
			WinHttpCloseHandle(curl->hProxyResolv);
#endif
		freezero(curl, sizeof(*curl));
	}
#ifdef _WIN32
	if (self->hHttpSession != INVALID_HANDLE_VALUE)
		WinHttpCloseHandle(self->hHttpSession);
	if (self->pairs[0] != SOCKET_ERROR) {
		event_del(&self->ev_pairs);
		closesocket(self->pairs[0]);
		closesocket(self->pairs[1]);
	}
#endif

	xfree(self);
}

/* callback from libcurl */
int
curl_libevent_set_events(CURL *easy, curl_socket_t sock, int action,
    void *userp, void *socketp)
{
	struct curl_libevent_sock
				*self = socketp;
	struct curl_libevent	*parent = userp;
	short			 evmask = 0;

	if (action == CURL_POLL_IN)
		evmask |= EV_READ | EV_PERSIST;
	else if (action == CURL_POLL_OUT)
		evmask |= EV_WRITE | EV_PERSIST;
	else if (action == CURL_POLL_INOUT)
		evmask |= EV_READ | EV_WRITE | EV_PERSIST;
	else if (action == CURL_POLL_REMOVE) {
		if (self) {
			TAILQ_REMOVE(&parent->socks, self, next);
			event_del(&self->ev_sock);
			freezero(self, sizeof(*self));
			curl_multi_assign(parent->handle, sock, NULL);
		}
		return (0);
	} else
		abort();

	if (self == NULL) {
		self = xcalloc(1, sizeof(*self));
		self->sock = sock;
		self->parent = parent;
		TAILQ_INSERT_TAIL(&parent->socks, self, next);
		curl_multi_assign(parent->handle, sock, self);
	} else
		event_del(&self->ev_sock);

	event_set(&self->ev_sock, sock, evmask, curl_libevent_on_event, self);
	if (parent->eb != NULL)
		event_base_set(parent->eb, &self->ev_sock);
	event_add(&self->ev_sock, NULL);

	return (0);
}

int
curl_libevent_set_timer(CURLM *multi, long timeout_ms, void *userp)
{
	struct curl_libevent	*self = userp;
	struct timeval		 tv;

	if (timeout_ms >= 0) {
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000UL;
		event_del(&self->ev_timer);
		event_add(&self->ev_timer, &tv);
	} else
		event_del(&self->ev_timer);

	return (0);
}

#ifndef _WIN32
void *
xcalloc(size_t nmemb, size_t size)
{
	void	*ret = calloc(nmemb, size);
	if (ret == NULL)
		err(1, "calloc");
	return (ret);
}
#endif

#ifdef _WIN32
void
curl_libevent_winhttp_callback(HINTERNET hInternet, DWORD_PTR dwContext,
    DWORD dwInternetStatus, LPVOID lpvStatusInformation,
    DWORD dwStatusInformationLength)
{
	struct curl_libevent_curl	*curl;
	struct curl_libevent		*self;
	struct pair_event		 ev;

	curl = dwContext;
	self = curl->parent;
	ev.context = dwContext;
	if (dwInternetStatus == WINHTTP_CALLBACK_STATUS_GETPROXYFORURL_COMPLETE)
		ev.dwError = NO_ERROR;
	else if (dwInternetStatus == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)
		ev.dwError = ((PWINHTTP_ASYNC_RESULT)lpvStatusInformation)->dwError;
	send(self->pairs[1], &ev, sizeof(ev), 0);
}

void
curl_libevent_on_pairs_event(intptr_t fd, short ev0, void *ctx)
{
	struct pair_event		 ev;
	SSIZE_T				 sz;
	struct curl_libevent		*self = ctx;
	struct curl_libevent_curl	*curl;
	WINHTTP_PROXY_RESULT		 proxyResult;
	char				*proxy;
	size_t				 proxylen;

	if ((sz = recv(fd, &ev, sizeof(ev), 0)) == -1)
		warnx("%s; recv() failed: %u", WSAGetLastError());
	if (sz != sizeof(ev)) {
		warnx("%s; recv() wrong size: %zd", sz);
		return;
	}
	curl = ev.context;
	if (ev.dwError != NO_ERROR) {
		warnx("%s: GetProxy failed %u", __func__, ev.dwError);
		goto out;
	}
	memset(&proxyResult, 0, sizeof(proxyResult));
	WinHttpGetProxyResult(curl->hProxyResolv, &proxyResult);
	if (proxyResult.cEntries <= 0) {
		warnx("%s: cEntries=%d", __func__, proxyResult.cEntries);
		goto out;
	}
	WINHTTP_PROXY_RESULT_ENTRY *result = &proxyResult.pEntries[0];
	if (result->fProxy && !result->fBypass) {
		proxylen = wcslen(result->pwszProxy);
		proxy = xcalloc(1, proxylen + 1);
		WideCharToMultiByte(CP_UTF8, 0, result->pwszProxy, -1,
		    proxy, proxylen + 1, NULL, FALSE);
		curl_easy_setopt(curl->handle, CURLOPT_PROXY, proxy);
		curl_easy_setopt(curl->handle, CURLOPT_PROXYPORT,
		    result->ProxyPort);
		CURL_LIBEVENT_DBG(("proxy %s:%d", proxy, result->ProxyPort));
		xfree(proxy);
	} else
		CURL_LIBEVENT_DBG(("proxy: none"));
	WinHttpFreeProxyResult(&proxyResult);

 out:
	WinHttpCloseHandle(curl->hProxyResolv);
	curl->hProxyResolv = INVALID_HANDLE_VALUE;
	curl_multi_add_handle(self->handle, curl->handle);
}

static void
freezero(void *ptr, size_t siz)
{
	if (ptr != NULL) {
		SecureZeroMemory(ptr, siz);
		xfree(ptr);
	}
}

static void
warnx(const char *msg, ...)
{
	va_list	ap;

	va_start(ap, msg);
	vwarnx(msg, ap);
	va_end(ap);
}

static void
vwarnx(const char *msg, va_list ap)
{
	vfprintf(stderr, msg, ap);
	fputc('\n', stderr);
}
#endif
