# ADR 0010: M5 Showcase Boundary

- Status: Accepted
- Date: 2026-07-18

## Context

M5 must demonstrate the two host shapes that motivated Scry: a GUI that pumps
an asynchronous chat turn inside its existing frame loop, and a game-like
application whose model observes and changes host state through tools. A useful
showcase must exercise the real public C++23 surface without quietly becoming a
second supported API, moving host lifecycle into the library, or adding GUI
dependencies to the package.

Dear ImGui is intentionally backend-agnostic. A complete desktop application
would also need a window system, renderer, platform backend, and event loop,
but selecting or owning those pieces would contradict API-005 and make a small
integration example carry a large platform matrix. The showcase therefore
needs a narrow panel boundary and a headless frame proof rather than another
application framework.

The NPC example also needs an honest side-effect boundary. Scry guarantees
at-most-once dispatch per tool-call ID within one accepted turn, but a failed
or cancelled turn commits no history and an application may submit a new turn.
The showcase must not imply that arbitrary durable game mutations gain
rollback or cross-turn idempotency merely by being exposed as tools.

## Decision

### Showcase-only boundary

M5 adds two opt-in C++23 examples. Both consume only the public `scry::scry`
target and public headers. They may define example-local controllers, views,
and domain objects, but none is installed, exported, or added to namespace
`scry`. M5 adds no Scry public API.

The examples remain host-owned integrations:

- the host creates and outlives the `Harness` and `Conversation`;
- the host calls `Harness::update()` from its existing loop;
- the host owns the ImGui context, window, renderer, platform backend, and
  event loop; and
- the host selects configuration and model endpoints, including an
  OpenAI-compatible local server with an empty API key.

This boundary is part of the showcase's value: the examples prove that Scry
fits an application loop instead of replacing one.

### ImGui chat panel

The chat panel is an example-local `scry_showcase::ChatPanel` constructed from
host-owned `Harness` and `Conversation` objects. Its immediate-mode `draw()`
surface demonstrates:

- non-blocking submission through the public asynchronous send path;
- streamed assistant text;
- completed, error, and cancelled terminal states; and
- an explicit Cancel control for an active turn.

The panel retains the active `Turn` needed to cancel it. Callback state is
shared independently of the panel object, while callbacks retain only a weak
reference plus a submission generation. Late events therefore cannot access a
destroyed panel or overwrite a newer submission. Destruction requests
cancellation and never waits; the host remains responsible for pumping and for
destroying the panel before the referenced Harness and Conversation.

A small example-private controller seam represents submit and cancel. Production
uses a controller backed only by the public Scry API; deterministic tests use a
fake controller to deliver text, completion, error, and cancellation without
network access, real time, or Scry internals. This is a showcase test seam, not
a new library abstraction.

### Deterministic NPC

The NPC showcase uses a fixed in-memory 5-by-5 grid. Coordinate `(0, 0)` is the
northwest corner, the NPC begins at `(2, 2)`, north/south change `y`, and
west/east change `x`. The host registers five explicit-schema, zero-argument
tools:

- `look`;
- `move_north`;
- `move_south`;
- `move_east`; and
- `move_west`.

Each schema is a closed empty object, and handlers reject arguments other than
`{}`. The tools explicitly retain the safe
`ToolExecution::app_thread` policy because they mutate host-owned world state.
`look` returns canonical JSON containing the grid bounds, position, and
available moves in deterministic order. A move returns its direction, whether
it moved, and the resulting position; a blocked boundary move also returns
`"reason":"boundary"` and leaves the position unchanged.

The example drives one model request from a host-owned update loop and reads a
local OpenAI-compatible endpoint from environment configuration. World state is
ephemeral and has no rollback. A durable side effect would require
application-owned idempotency keys or reconciliation, exactly as required by
LOOP-008.

### Dear ImGui dependency

Dear ImGui is permitted only as a build-time showcase dependency:

- version `v1.92.8`, pinned to commit
  `8936b58fe26e8c3da834b8f60b06511d537b4c63`;
- MIT licensed;
- fetched and compiled only when `SCRY_BUILD_IMGUI_SHOWCASE=ON`, which defaults
  to `OFF`; and
- limited to core Dear ImGui sources, with no GLFW, SDL, OpenGL, Metal, Vulkan,
  or other platform/renderer backend.

The dependency must not appear in Scry public headers, target interfaces,
installed files, package exports, or runtime dependencies. A normal core build
must not download or discover Dear ImGui.

### Acceptance gates

M5 is complete only when one shared showcase gate:

- builds the showcase code with the repository warnings-as-errors policy;
- runs deterministic NPC world and registration tests;
- runs fake-controller panel tests for send, streaming, completion, error,
  cancellation, stale callbacks, and destruction;
- compiles and links the real panel with the pinned Dear ImGui sources and
  executes one headless ImGui frame; and
- audits a clean core install/package to prove that no showcase source,
  header, target, dependency, or build option leaks into the consumer surface.

The gate passes locally and in hosted CI. The M5 acceptance criteria are
satisfied.

## Consequences

The panel is deliberately an embeddable widget, not a runnable desktop shell.
Consumers bring whichever ImGui backend and host loop they already use. A
future maintained standalone demo may choose a backend, but it must remain
outside the Scry package boundary and earn its own portability contract.

The NPC world is intentionally small and deterministic. It proves explicit
schemas, app-thread tool ownership, automatic tool-result resend, and host
state observation without inventing an engine abstraction. Persistence,
rollback, pathfinding, parallel actors, and production idempotency remain
application concerns.

Dear ImGui does not join libcurl and Glaze as a Scry runtime dependency. If a
future public UI component is proposed, it requires a separate API,
installation, dependency, and platform-support decision rather than growing
out of this showcase implicitly.
