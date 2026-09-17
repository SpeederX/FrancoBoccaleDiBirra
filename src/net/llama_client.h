#ifndef FB_LLAMA_CLIENT_H
#define FB_LLAMA_CLIENT_H

/* Returns 1 if llama-server on 127.0.0.1:port responds successfully to
   /health, 0 otherwise (including "not up yet" and "connection refused"). */
int fb_llama_health(int port, int timeout_ms);

#endif /* FB_LLAMA_CLIENT_H */
