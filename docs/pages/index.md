# Scry documentation {#mainpage}

Scry is a C++ LLM harness for applications that own their main loop. Its documentation is split
into two deliberately different views: the contract an application can depend on, and the
implementation a contributor needs to understand.

@htmlonly
<nav class="scry-doc-paths" aria-label="Documentation views">
  <a class="scry-doc-card scry-doc-card-api" href="api_reference.html">
    <span class="scry-doc-card-kicker">Build with Scry</span>
    <span class="scry-doc-card-title">API Reference</span>
    <span class="scry-doc-card-copy">Stable C++23 concepts, lifetime rules, errors, callbacks,
      and the optional experimental C++26 reflection surface.</span>
    <span class="scry-doc-card-link">Explore the public contract <span aria-hidden="true">&rarr;</span></span>
  </a>
  <a class="scry-doc-card scry-doc-card-source" href="source_documentation.html">
    <span class="scry-doc-card-kicker">Contribute to Scry</span>
    <span class="scry-doc-card-title">Source Documentation</span>
    <span class="scry-doc-card-copy">Internal types, ownership boundaries, runtime flow,
      provider adapters, protocols, and browsable implementation sources.</span>
    <span class="scry-doc-card-link">Understand the implementation <span aria-hidden="true">&rarr;</span></span>
  </a>
</nav>
@endhtmlonly

## Choose the layer you need

The @ref api_reference "API Reference" is the right starting point when embedding Scry in an
application. It is curated from the maintained headers under `include/` and distinguishes the
stable C++23 contract from the experimental reflection component.

The @ref source_documentation "Source Documentation" is for maintainers, reviewers, and curious
readers. It connects the architecture to the declarations and implementation under `src/`, with
full source browsing and relationship diagrams.

## The project in one minute

1. An application configures a @ref scry::Harness and submits a turn without surrendering its
   main loop.
2. A worker actor performs provider and transport work while the application keeps running.
3. @ref scry::Harness::update "Harness::update()" pumps events and tool handlers on the calling
   thread.
4. A successful terminal event commits conversation history atomically; failure and cancellation
   commit nothing.

@htmlonly
<div class="scry-note scry-note-contract">
  <strong>Contract compass.</strong> Generated source documentation explains the current code; it
  does not redefine product behavior. The RFC-2119 rows in <code>docs/requirements.md</code> are
  normative, followed by <code>docs/design/</code> and <code>docs/architecture/</code> for rationale
  and boundaries.
</div>
@endhtmlonly

Scry's stable library surface targets C++23. C++26 reflection is isolated behind
`scry::reflection` and separate build flags so core-only consumers do not inherit experimental
headers, compiler flags, or dependencies.
