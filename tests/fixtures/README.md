# Provider golden fixtures

**These payloads were hand-synthesized from the public Anthropic Messages and
OpenAI Chat Completions streaming references. None of them was captured from a
live service.** They were written to match the documented wire shapes, then
kept honest by the adapter tests that read them. Treat them as executable
documentation of what Scry believes the wire looks like, not as evidence of
what a provider actually sent.

They are loaded through `tests/provider/fixture_support.hpp`, which resolves
`SCRY_ANTHROPIC_FIXTURE_DIR` and `SCRY_OPENAI_FIXTURE_DIR`, both set in
`tests/provider/CMakeLists.txt`. Assertions against them are semantic: request
goldens are compared after canonicalization (parse then re-encode), so member
order and whitespace are not part of the contract.

## What each file pins

| File | Pins | Read by |
|---|---|---|
| `anthropic/request.json` | The Anthropic Messages request body Scry builds from a minimal neutral request: `system` alongside `messages` rather than inside them, block-shaped user content, `stream: true`, and the `max_tokens`/`temperature`/`top_p` sampling spelling | `tests/provider/provider_tests.cpp` |
| `anthropic/stream.sse` | A complete Anthropic streaming response, decoded one byte at a time: `message_start` with input usage, an ignorable `ping`, a text block opened by `content_block_start`, two `text_delta` fragments, `content_block_stop`, `message_delta` carrying `stop_reason` and output usage, and `message_stop` | `tests/provider/stream_tests.cpp` |
| `openai/request.json` | The OpenAI-compatible Chat Completions request body: the system message, merged user text, an assistant message carrying `tool_calls` with stringified arguments, one `role: "tool"` message per result, sampling fields, `stream_options.include_usage`, and the function-tool list | `tests/provider/openai_request_tests.cpp` |

No fixture contains a credential: API keys travel in request headers
(`x-api-key`, `authorization`), which the adapter tests assert on separately and
which never enter a request body.

The disposable self-signed key and certificate under `tls/` follow the same
"deliberate test fixture" convention; see [`tls/README.md`](tls/README.md).

There is no OpenAI streaming golden file: `openai_stream_tests.cpp` builds each
chunk from a template so it can vary one field at a time, and the end-to-end
OpenAI streams live next to the integration tests that assert on them.

## Re-capturing a fixture from a real service

Nothing here requires live capture, but when API keys are available a real
payload is strictly better evidence. The recipe:

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

Before a captured payload is checked in:

1. **Redact.** Remove or replace every credential and correlation identifier:
   the `x-api-key` and `authorization` request headers, and the `request-id`,
   `x-request-id`, `anthropic-organization-id`, `openai-organization`, and
   `set-cookie` response headers. The tests use the placeholder keys
   `sanitized-test-key` (Anthropic) and `sanitized-key` (OpenAI), so a real key
   must never be dumped alongside a fixture. Account identifiers embedded in a
   body or a stream — organization, project, user, or message ids — go too; the
   checked-in stream uses `msg_stream_sanitized`.
2. **Trim.** A fixture pins one wire shape. Drop unrelated blocks and keep the
   stream short enough to read; the byte-at-a-time decode in `stream_tests.cpp`
   is O(bytes) in test time.
3. **Keep the assertions passing, or update them in the same change.** A
   re-captured fixture that needs different assertions is a real finding about
   the provider's wire format: change the fixture, the assertions, and the
   adapter together, and say so in the pull request. Silently loosening an
   assertion to accommodate a new capture defeats the point of the golden.
4. **Update this file.** The table above and this provenance statement must
   describe what is actually checked in — if a fixture becomes a real capture,
   say which service and when.
