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

You declare the tools. You do not write them. `World` in `world.hpp` is a
toolbox. Each member function of `World` with the `scry::reflection::tool`
annotation becomes a tool, and the tool has the name of the function.
`harness.tools().add(world)` registers all five tools against the shared
`World`. The host also keeps this `World` to print the final state. Scry makes
the schema of each tool from its parameters. These tools have no parameters.
Thus, the model sends `{}`, and Scry rejects all other arguments before the NPC
moves. Scry encodes the `Observation` and `MoveOutcome` results from their
members. Registration is atomic. If one of the five names is already in use,
Scry registers none of the five tools.

The world is temporary by design. If a model turn fails or is cancelled, the
example does not roll back the movement that already occurred. A real
application that exposes durable side effects must supply its own idempotency
keys, persistence, and reconciliation policy.
