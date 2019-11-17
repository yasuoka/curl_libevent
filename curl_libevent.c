#include <err.h>
#include <event.h>
#include <unistd.h>
#include <stdlib.h>

#include <curl/curl.h>

void		*xcalloc(size_t , size_t);

/* curl glue */
struct curl_libevent;
static struct curl_libevent
		*curl_libevent_create(void);
static void	 curl_libevent_perform(struct curl_libevent *, CURL *,
		    void (*on_done)(void *, CURLMsg *));
static void	 curl_libevent_on_event(int, short, void *);
static void	 curl_libevent_on_timer(int, short, void *);
static void	 curl_libevent_events(struct curl_libevent *);
static void	 curl_libevent_destroy(struct curl_libevent *);
static int	 curl_libevent_set_events(CURL *, curl_socket_t, int, void *,
		    void *);
static int	 curl_libevent_set_timer(CURLM *, long , void *);

/************************************************************************
 * glue for libevent and curl
 ************************************************************************/
struct curl_libevent_sock;
struct curl_libevent_curl;

struct curl_libevent {
	CURLM			*handle;
	struct event		 ev_timer;
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
	CURL			 *handle;
	void			(*on_done)(void *, CURLMsg *);
	TAILQ_ENTRY(curl_libevent_curl)
				  next;
};

struct curl_libevent *
curl_libevent_create(void)
{
	struct curl_libevent	*self;

	self = xcalloc(1, sizeof(*self));
	self->handle = curl_multi_init();
	TAILQ_INIT(&self->curls);
	TAILQ_INIT(&self->socks);

	curl_multi_setopt(self->handle, CURLMOPT_SOCKETFUNCTION,
	    curl_libevent_set_events);
	curl_multi_setopt(self->handle, CURLMOPT_TIMERFUNCTION,
	    curl_libevent_set_timer);
	curl_multi_setopt(self->handle, CURLMOPT_SOCKETDATA, self);
	curl_multi_setopt(self->handle, CURLMOPT_TIMERDATA, self);
	evtimer_set(&self->ev_timer, curl_libevent_on_timer, self);

	return (self);
}

void
curl_libevent_perform(struct curl_libevent *self, CURL *handle,
    void (*on_done)(void *, CURLMsg *))
{
	struct curl_libevent_curl *curl;

	curl = xcalloc(1, sizeof(*curl));
	curl->handle = handle;
	curl->on_done = on_done;
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
		freezero(curl, sizeof(*curl));
	}
	free(sock);
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

void *
xcalloc(size_t nmemb, size_t size)
{
	void	*ret = calloc(nmemb, size);
	if (ret == NULL)
		err(1, "calloc");
	return (ret);
}
