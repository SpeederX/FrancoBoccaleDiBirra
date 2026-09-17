#ifndef FB_INTENT_SCHEMA_H
#define FB_INTENT_SCHEMA_H

/*
 * Confirmed empirically against a real resident Qwen3.5-0.8B instance
 * (llama-server --reasoning off): without an explicit "prefer unknown"
 * instruction, the model reliably mis-fires on unrelated input (a weather
 * question got classified as load_model). This wording measurably fixed
 * that case — but it's still a small model, so command_dispatch.c's own
 * exact-name-match requirements for the destructive verbs (unload/remove)
 * remain the real safety net, not this prompt.
 */
#define FB_INTENT_SYSTEM_PROMPT                                                                  \
    "Classify the user's message into exactly one intent. Only use load_model, unload_model, "   \
    "add_model, remove_model, or set_lang if the message is CLEARLY and EXPLICITLY an "           \
    "instruction about managing local AI models (loading, unloading, adding, removing a model, " \
    "or setting a transcription language). If the message is small talk, a question, or about "  \
    "anything else, you MUST use \"unknown\". When truly unsure, always prefer \"unknown\" over " \
    "guessing."

/* OpenAI-compatible response_format, verified to work against llama-server
   (json_schema + strict:true). "path" is part of the schema for symmetry
   with /add's syntax, but is never trusted for add_model — see
   intent_brain.c. */
#define FB_INTENT_RESPONSE_FORMAT_JSON                                                           \
    "{"                                                                                          \
    "\"type\": \"json_schema\","                                                                 \
    "\"json_schema\": {"                                                                         \
    "\"name\": \"intent\","                                                                      \
    "\"strict\": true,"                                                                          \
    "\"schema\": {"                                                                               \
    "\"type\": \"object\","                                                                       \
    "\"properties\": {"                                                                           \
    "\"intent\": {\"type\": \"string\", \"enum\": [\"load_model\", \"unload_model\", "            \
    "\"add_model\", \"remove_model\", \"set_lang\", \"unknown\"]},"                               \
    "\"model\": {\"type\": [\"string\", \"null\"]},"                                              \
    "\"path\": {\"type\": [\"string\", \"null\"]},"                                               \
    "\"lang\": {\"type\": [\"string\", \"null\"]}"                                                \
    "},"                                                                                          \
    "\"required\": [\"intent\", \"model\", \"path\", \"lang\"],"                                  \
    "\"additionalProperties\": false"                                                             \
    "}"                                                                                            \
    "}"                                                                                            \
    "}"

#endif /* FB_INTENT_SCHEMA_H */
