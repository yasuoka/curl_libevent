curl\_libevent - a glue of libcurl and libevent
===============================================

curl\_libevent is a glue that makes your program can use
[libcurl(3)](https://curl.se/libcurl/) asynchromously with
[libevent(3)](https://libevent.org/).

- Supports Windows
  - Open `curl_libevent.sln` in win32 directory by Visual Studio
  - Configure proxy automatically by using WinHTTP.  Call
    `curl_libevent_set_auto_proxy_config(, true)`

## Example

```c
#include "curl_libevent.h"

void
curl_on_done(void *ctx, CURLMsg *msg)
{
	/* callback when the perform is done */
}

int
main(int argc, char *argv[])
{
	struct event_base	*eb;
	struct curl_libevent	*evcurl;
	    :
	/* initialize */
	eb = event_init();
	evcurl = curl_libevent_create(eb);	// Add this
	curl_global_init(CURL_GLOBAL_DEFAULT);
	    :
	// Configure proxy automatically if needed
	curl_libevent_set_auto_proxy_config(evcurl, true);
	event_loop();
	    :
	/* finalize */
	curl_libevent_destroy(evcurl);		// Add this
	curl_global_cleanup();
}

/* somewhere in your application */
	:
	CURL	*curl;

	curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_URL, "https://....");
	    :
	/* call curl_libevent_perform() like curl_perform */
	curl_libevent_perform(evcurl, curl, curl_on_done);
	/* curl_on_done will be called when the perform is done */

	    :
```

See [test.c](./test.c) for a complete example.

