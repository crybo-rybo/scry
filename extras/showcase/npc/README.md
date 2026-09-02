# NPC showcase

This one-shot example lets an OpenAI-compatible model control an in-memory NPC
on a deterministic 5 by 5 grid. The host owns the `scry::Harness`, the world,
and the `Harness::update()` loop. All five tools run on the application thread:
`look`, `move_north`, `move_south`, `move_east`, and `move_west`.

Configure a local or hosted OpenAI-compatible endpoint, then run the
`scry_npc_showcase` target:

```sh
export SCRY_LOCAL_MODEL_BASE_URL=http://127.0.0.1:11434/v1
export SCRY_LOCAL_MODEL_MODEL=qwen3:8b
# SCRY_LOCAL_MODEL_API_KEY is optional for local servers.
./build/showcase/scry_npc_showcase
```

Pass command-line arguments to replace the default movement request.

The example disables model reasoning through Scry's OpenAI-compatible request
configuration. With the default prompt it prints each completed tool call
(`look`, `move_north`, then `move_east`), the model's final answer, and the final
world state. A truncated response, empty final answer, or response that executes
no NPC tool exits nonzero instead of presenting a no-op as success. The selected
server must support `reasoning_effort: "none"`; leave `ReasoningMode` at its
default in applications whose endpoint does not support that optional field.

`register_world_tools()` expects a fresh registry for these five names.
`ToolRegistry` is additive-only, so a collision on a later name can leave
earlier showcase tools registered; discard that Harness after registration
failure instead of assuming rollback.

The world is intentionally ephemeral. A failed or cancelled model turn does not
roll back movement that already occurred. Real applications that expose durable
side effects must supply their own idempotency keys, persistence, and
reconciliation policy.
