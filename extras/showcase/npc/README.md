# NPC showcase

This example runs one time and then exits. It lets an OpenAI-compatible model
control an in-memory NPC on a deterministic 5 by 5 grid. The host owns the
`scry::Harness`, the world, and the `Harness::update()` loop. All five tools run
on the application thread: `look`, `move_north`, `move_south`, `move_east`, and
`move_west`.

Build the standalone showcase from the repository root:

```sh
cmake -S extras/showcase -B build/showcase -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++-16
cmake --build build/showcase --target scry_npc_showcase
```

Configure a local or hosted OpenAI-compatible endpoint. Then run the executable:

```sh
export SCRY_LOCAL_MODEL_BASE_URL=http://127.0.0.1:11434/v1
export SCRY_LOCAL_MODEL_MODEL=qwen3:8b
# SCRY_LOCAL_MODEL_API_KEY is optional for local servers.
./build/showcase/scry_npc_showcase
```

To replace the default movement request, pass command-line arguments.

The example disables model reasoning with the OpenAI-compatible request
configuration of Scry. The default prompt asks for `look`, `move_north`, and
`move_east`. The model selects which calls it makes. The example prints the
calls that it sees and the text that the model streams. If the model streamed no
text, the example prints the final answer instead. It also prints the final
world state.

The example exits nonzero if the response is truncated or the final answer is
empty. It also exits nonzero if the response executes no NPC tool. Thus, it does
not show a no-op as a success. The server that you select must support
`reasoning_effort: "none"`. If the endpoint of your application does not
support that optional field, keep `ReasoningMode` at its default value.

`register_world_tools()` expects a new registry for these five names.
`ToolRegistry` is additive-only. Thus, if a later name causes a collision, the
earlier showcase tools can stay registered. If registration fails, discard that
Harness. Do not assume a rollback.

The world is temporary by design. If a model turn fails or is cancelled, the
example does not roll back the movement that already occurred. A real
application that exposes durable side effects must supply its own idempotency
keys, persistence, and reconciliation policy.
