# Provider golden fixtures

**These payloads were made by hand from the public streaming references for
Anthropic Messages and OpenAI Chat Completions. The two exceptions are the
`request_tool_history.json` goldens. These two are captures from the encoder of
Scry itself. No payload is a capture from a live service.** The authors wrote
the payloads to match the documented wire shapes. The adapter tests that read
the payloads then keep them honest. Use them as executable documentation of how
Scry thinks the wire looks. Do not use them as evidence of what a provider
actually sent.

`tests/provider/fixture_support.hpp` loads the payloads. It resolves
`SCRY_ANTHROPIC_FIXTURE_DIR` and `SCRY_OPENAI_FIXTURE_DIR`.
`tests/provider/CMakeLists.txt` sets these two values. The assertions against
the payloads are semantic. The tests compare request goldens after
canonicalization (parse, then encode again). Thus, member order and whitespace
are not part of the contract.

## What each file pins

| File | Pins | Read by |
|---|---|---|
| `anthropic/request.json` | The Anthropic Messages request body that Scry builds from a minimal neutral request: `system` next to `messages`, not inside them; user content in block shape; `stream: true`; and the `max_tokens`/`temperature`/`top_p` spelling of the sampling fields | `tests/provider/provider_tests.cpp` |
| `anthropic/stream.sse` | A complete Anthropic streaming response that the test decodes one byte at a time: `message_start` with input usage, a `ping` that the decoder can ignore, a text block that `content_block_start` opens, two `text_delta` fragments, `content_block_stop`, `message_delta` with `stop_reason` and output usage, and `message_stop` | `tests/provider/stream_tests.cpp` |
| `openai/request.json` | The OpenAI-compatible Chat Completions request body: the system message, merged user text, an assistant message with `tool_calls` that have their arguments as strings, one `role: "tool"` message for each result, sampling fields, `stream_options.include_usage`, and the function-tool list | `tests/provider/openai_request_tests.cpp` |
| `anthropic/request_tool_history.json`, `openai/request_tool_history.json` | The body in each dialect for one request with text, two tool calls with nested-object arguments, one plain and one error tool result, and two tools with nested schemas. These goldens are different from the others: they are captures from Scry itself. The generic-tree encoder emitted them before the encoders changed to splice stored canonical text. Thus, they pin that the rewrite is byte-for-byte equivalent | `tests/provider/request_encoding_tests.cpp` |

No fixture contains a credential. API keys go in request headers
(`x-api-key`, `authorization`). The adapter tests assert on these headers
separately. These headers never go into a request body.

The disposable self-signed key and certificate under `tls/` follow the same
"deliberate test fixture" convention. Refer to [`tls/README.md`](tls/README.md).

There is no golden file for OpenAI streaming. `openai_stream_tests.cpp` builds
each chunk from a template, so it can change one field at a time. The
end-to-end OpenAI streams are next to the integration tests that assert on them.

## Capture a fixture again from a real service

No fixture here must come from a live capture. But if you have API keys, a real
payload is always better evidence. Use this procedure:

```sh
# Anthropic Messages (streaming)
curl --no-buffer https://api.anthropic.com/v1/messages \
  -H "x-api-key: $ANTHROPIC_API_KEY" \
  -H "anthropic-version: 2023-06-01" \
  -H "content-type: application/json" \
  --data @tests/fixtures/anthropic/request.json \
  --dump-header /dev/stderr \
  > /tmp/anthropic-stream.sse

# OpenAI-compatible Chat Completions (streaming)
curl --no-buffer https://api.openai.com/v1/chat/completions \
  -H "authorization: Bearer $OPENAI_API_KEY" \
  -H "content-type: application/json" \
  --data @tests/fixtures/openai/request.json \
  --dump-header /dev/stderr \
  > /tmp/openai-stream.sse
```

Before you check in a captured payload, do these steps:

1. **Redact.** Remove or replace each credential and each correlation
   identifier. These include the `x-api-key` and `authorization` request
   headers. They also include the `request-id`, `x-request-id`,
   `anthropic-organization-id`, `openai-organization`, and `set-cookie`
   response headers. The tests use the placeholder keys `sanitized-test-key`
   (Anthropic) and `sanitized-key` (OpenAI). Thus, never dump a real key
   together with a fixture. Also remove or replace the account identifiers in
   a body or a stream: organization, project, user, or message ids. The
   checked-in stream uses `msg_stream_sanitized`.
2. **Trim.** A fixture pins one wire shape. Remove unrelated blocks. Keep the
   stream short enough to read. The byte-at-a-time decode in `stream_tests.cpp`
   takes O(bytes) test time.
3. **Make sure that the assertions pass, or update them in the same change.**
   If a fixture that you capture again needs different assertions, this is a
   real finding about the wire format of the provider. Change the fixture, the
   assertions, and the adapter together. Write this in the pull request. Do not
   silently loosen an assertion to accept a new capture. That removes the value
   of the golden.
4. **Update this file.** The table above and this provenance statement must
   describe the files that are actually checked in. If a fixture becomes a real
   capture, write the service and the date of the capture.
