# NPC showcase

This one-shot example lets an OpenAI-compatible model control an in-memory NPC
on a deterministic 5 by 5 grid. The host owns the `scry::Harness`, the world,
and the `Harness::update()` loop. All five tools run on the application thread:
`look`, `move_north`, `move_south`, `move_east`, and `move_west`.

Build the standalone showcase from the repository root:

```sh
cmake -S extras/showcase -B build/showcase -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++-16
cmake --build build/showcase --target scry_npc_showcase
```

Configure a local or hosted OpenAI-compatible endpoint, then run the executable:

```sh
export SCRY_LOCAL_MODEL_BASE_URL=http://127.0.0.1:11434/v1
export SCRY_LOCAL_MODEL_MODEL=qwen3:8b
# SCRY_LOCAL_MODEL_API_KEY is optional for local servers.
./build/showcase/scry_npc_showcase
```

Pass command-line arguments to replace the default movement request.

The example disables model reasoning through Scry's OpenAI-compatible request
configuration. The default prompt asks for `look`, `move_north`, and
`move_east`; the model chooses which calls to issue. The example prints the
calls it observes, streamed text (or the final answer if no text was streamed),
and the final world state. A truncated response, empty final answer, or response that executes
no NPC tool exits nonzero instead of presenting a no-op as success. The selected
server must support `reasoning_effort: "none"`; leave `ReasoningMode` at its
default in applications whose endpoint does not support that optional field.

The tools are declared, not written: `World` in `world.hpp` is a toolbox, and
each of its member functions annotated with `scry::reflection::tool` becomes a
tool named after the function. `harness.tools().add(world)` registers all five
against the shared `World`, which the host also keeps to print the final state.
Scry generates each tool's schema from its parameters (none, so the model sends
`{}` and anything else is rejected before the NPC moves) and encodes the
`Observation` and `MoveOutcome` results from their members. Registration is
atomic: if any of the five names were already taken, none of them would be
registered.

The world is intentionally ephemeral. A failed or cancelled model turn does not
roll back movement that already occurred. Real applications that expose durable
side effects must supply their own idempotency keys, persistence, and
reconciliation policy.
