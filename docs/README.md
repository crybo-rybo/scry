# Documentation

Start with the [design overview](design/overview.md). The
[requirements register](requirements.md) is normative when explanatory prose
and a binding requirement disagree.

## Design

- [Overview](design/overview.md)
- [Public API and agentic loop](design/public-api.md)
- [Runtime behavior](design/runtime-behavior.md)
- [Tools and providers](design/tools-and-providers.md)
- [Future directions and scope](design/roadmap.md)

## Architecture

- [Overview](architecture/overview.md)
- [Runtime](architecture/runtime.md)
- [Tools, providers, and transport](architecture/tools-and-providers.md)
- [Errors and dependencies](architecture/dependencies-and-errors.md)
- [Quality and evolution](architecture/quality-and-evolution.md)

## Development

- [Principles and testing](development/principles-and-testing.md)
- [Quality gates](development/quality-gates.md)
- [Performance profiling](development/performance-profiling.md)
- [Workflow](development/workflow.md)

## Releases

- [v0.2.0](releases/v0.2.0.md)
- [v0.1.1](releases/v0.1.1.md)

## Generated reference site

The generated site has two curated entry points:

- **API Reference** covers Scry's exported C++23 API and its optional C++26 reflection
  component. This is the consumer-facing contract.
- **Source Documentation** maps the implementation for contributors: private header contracts,
  ownership and thread-affinity notes, subsystem starting points, include relationships, and full
  browsable source.

The code index is intentionally bounded to maintained C++ files under `include/` and `src/`.
Tests, test support outside `src/`, benchmarks, examples, and build tooling are not Doxygen input.
Header-declared private and internal entities are referenceable; translation-unit-local helpers
remain visible in the source browser without being promoted into standalone reference pages.

The current `main` reference is published at
[crybo-rybo.github.io/scry](https://crybo-rybo.github.io/scry/).

Build it from the repository root:

```sh
./scripts/ci-docs.sh
```

The site is written to `build/docs/html/index.html`. The build requires Doxygen 1.9.8 or newer
and Graphviz `dot`. Doxygen 1.9.8 is the minimum because it is the version provided by the pinned
Ubuntu 24.04 CI environment; Graphviz is used only for generated SVG relationship diagrams. Both
are build-only documentation tools and do not enter Scry's compiled, installed, or exported
dependency surface. Hosted CI retains every generated site as an artifact and deploys successful
non-pull-request `main` builds to GitHub Pages.

Doxygen diagnostics remain visible in CI, but comment coverage and quality are review obligations
rather than a separate generation gate. Comments on implementation details should explain
invariants, ownership, bounds, thread affinity, and error behavior instead of merely translating
the C++ spelling into prose.
