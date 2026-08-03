# ADR 0010: M5 Showcase Boundary

- Status: Accepted
- Date: 2026-07-18
- Amended: 2026-08-02

## Context

M5 must demonstrate a GUI that pumps an asynchronous chat turn inside its
existing frame loop. A useful showcase must exercise the real public C++23
surface without quietly becoming a second supported API, moving host lifecycle
into the library, or adding GUI dependencies to the package.

Dear ImGui is intentionally backend-agnostic. A complete desktop application
would also need a window system, renderer, platform backend, and event loop,
but selecting or owning those pieces would contradict API-005 and make a small
integration example carry a large platform matrix. The showcase therefore
needs a narrow panel boundary and a headless frame proof rather than another
application framework.

## Decision

### Showcase-only boundary

M5 adds one opt-in C++23 example. It consumes only the public `scry::scry`
target and public headers. It may define example-local controllers and views,
but none is installed, exported, or added to namespace `scry`. M5 adds no Scry
public API.

The example remains a host-owned integration:

- the host creates and outlives the `Harness` and `Conversation`;
- the host calls `Harness::update()` from its existing loop;
- the host owns the ImGui context, window, renderer, platform backend, and
  event loop; and
- the host selects configuration and model endpoints, including an
  OpenAI-compatible local server with an empty API key.

This boundary is part of the showcase's value: the example proves that Scry
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

Dear ImGui does not join libcurl and Glaze as a Scry runtime dependency. If a
future public UI component is proposed, it requires a separate API,
installation, dependency, and platform-support decision rather than growing
out of this showcase implicitly.
