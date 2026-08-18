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

- [v0.1.1](releases/v0.1.1.md)

## API reference

The generated reference covers Scry's exported C++23 API and its optional C++26 reflection
component. Private implementation headers and namespaces are deliberately excluded.

Build it from the repository root:

```sh
./scripts/ci-docs.sh
```

The site is written to `build/docs/html/index.html`. The build requires Doxygen 1.9.8 or newer
and Graphviz `dot`. Doxygen 1.9.8 is the minimum because it is the version provided by the pinned
Ubuntu 24.04 CI environment; Graphviz is used only for generated SVG relationship diagrams. Both
are build-only documentation tools and do not enter Scry's compiled, installed, or exported
dependency surface.

Documentation warnings are errors. Public declarations, parameters, return values, enum values,
and cross-references must stay complete enough for a clean generation pass.
