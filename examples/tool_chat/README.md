# Interactive tool chat

This developer-facing example starts a persistent terminal conversation against
an OpenAI-compatible model and makes every model-selected tool call visible. It
registers three tools:

- `get_current_datetime({})`
- `add({"a": number, "b": number})`
- `multiply({"a": number, "b": number})`

The launcher defaults to a local Ollama server and `qwen3:8b`:

```sh
./scripts/run-tool-chat.sh
```

Or, with [`just`](https://github.com/casey/just):

```sh
just tool-chat
```

Override the endpoint, model, optional API key, or diagnostic log path with
`SCRY_LOCAL_MODEL_BASE_URL`, `SCRY_LOCAL_MODEL_MODEL`,
`SCRY_LOCAL_MODEL_API_KEY`, and `SCRY_LOG_FILE`. For example:

```sh
SCRY_LOCAL_MODEL_MODEL=qwen3:14b ./scripts/run-tool-chat.sh
```

The terminal prints streamed assistant text, model-supplied tool arguments,
tool results, and the number of provider requests used by each turn. Internal
Scry lifecycle diagnostics default to `build/dev-logging/tool-chat.log`; set
`SCRY_LOG_FILE=/dev/stderr` to interleave them with the chat transcript.

Use `/tools`, `/reset`, `/help`, and `/quit` while chatting. This is an
exploratory developer tool, not a deterministic acceptance gate. Use
`scripts/ci-local-model.sh` when a bounded pass/fail live-model check is needed.
