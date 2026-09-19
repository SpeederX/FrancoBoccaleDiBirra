#ifndef FB_OPENAI_CLIENT_H
#define FB_OPENAI_CLIENT_H

/* Was llama_client.h/fb_llama_health -- renamed once backend_mgr grew
   vLLM/SGLang support (see backend_types.h's FbBackendKind), since /health
   is a plain OpenAI-compatible-server convention all three engines share
   identically. Nothing here is llama.cpp-specific; it never was. */

/* Returns 1 if the backend on 127.0.0.1:port responds successfully to
   /health, 0 otherwise (including "not up yet" and "connection refused").
   Works unchanged for llama-server, vLLM, and SGLang -- all three expose
   the same /health convention on their OpenAI-compatible HTTP server. */
int fb_backend_health(int port, int timeout_ms);

#endif /* FB_OPENAI_CLIENT_H */
