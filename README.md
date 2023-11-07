# sneed
SNEED SERVER (non-select version) - C89 ANSI POSIX1 compliant concurrent file server with directory listing.

#### Since select is not used, beware of high-cpu usage while waiting on socket. It's not limited to 1024 concurrent connections at least. To lower cpu cycles waiting on socket, uncomment /* usleep_milliseconds(100); */ line and play with value. Sleeping tanks the serving speed though.

```
Usage: [-d directory] [-i initialfile] [-p port]

-d directory to serve
-i initialfile to serve at "/"
-p port

Compile: cc sneed.c
```
