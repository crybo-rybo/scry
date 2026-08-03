# API documentation

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
