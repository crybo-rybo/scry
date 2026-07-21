# ADR 0007: M3 Reflected Tool Contract

- Status: Accepted (verification mechanics amended by
  [ADR 0011](0011-absolute-quality-gates.md) and
  [ADR 0012](0012-release-infrastructure-simplification.md))
- Date: 2026-07-18

## Context

M2 deliberately accepts an explicit JSON-object schema and a move-only
`Json -> Result<Json>` handler. M3 adds a C++26 ergonomics layer without
changing that runtime registry, its immutable accepted-turn snapshots, or its
app-thread dispatch policy.

The original M3 requirements fixed the use of P2996, compile-time schemas,
default-member detection, concept-constrained registration, descriptions, and
typed returns, but intentionally deferred the concrete type mapping and
marshalling policy. Those choices must be fixed before implementation so the
schema, decoder, diagnostics, package boundary, and tests describe one
contract.

The reflection layer also has two non-negotiable boundaries inherited from the
stable library:

- the C++23 core must build, install, and run without reflection; and
- Glaze remains private implementation machinery. Neither its headers nor its
  types may enter Scry's public include path.

## Decision

### Public surface and lowering

The optional reflection component exposes the following Scry-owned surface in
`scry::reflection`:

```cpp
struct ToolMetadata {
  std::string name{};
  std::string description{};
};

struct parameter_description {
  std::string_view member{};
  std::string_view text{};
};

template <class Args> struct tool_traits {
  static constexpr std::array<parameter_description, 0> descriptions{};
};

template <std::size_t N> struct description {
  char text[N];
};

template <class T> concept SupportedValue = /* specified below */;
template <class T> concept ToolArguments = /* specified below */;

template <ToolArguments Args>
inline constexpr std::string_view input_schema_v = /* generated schema */;

template <ToolArguments Args, class Handler>
[[nodiscard]] Status add(ToolRegistry&, ToolMetadata, Handler&&);
```

`add<Args>` generates the schema and a typed marshalling wrapper, then calls the
existing explicit `ToolRegistry::add`. It creates no second registry, snapshot
type, or dispatch path. The free function keeps P2996 declarations out of the
stable `ToolRegistry` class definition.

The handler is invoked with `std::move(args)`. A handler is accepted when it is
invocable with that expression and returns either a direct `SupportedValue` or
`Result<SupportedValue>`. The reflected overload does not accept `void`,
`Status`, raw `Json`, references, futures, or awaitables. Applications needing
a dynamic or otherwise unsupported boundary continue to use the explicit
`Json -> Result<Json>` overload.

### Generated-schema dialect and canonical form

Generated input schemas use a deliberately closed, provider-neutral subset of
JSON Schema 2020-12. Scry guarantees only the keywords it emits:
`additionalProperties`, `anyOf`, `description`, `enum`, `items`, `maxItems`,
`minItems`, `minimum`, `maximum`, `properties`, `required`, and `type`.
Generated schemas do not emit `$schema`, `$id`, `$ref`, `$defs`, `title`,
`default`, provider extensions, or arbitrary validation annotations.

Every generated schema is one minified canonical JSON object:

- JSON object keys, including names inside `properties`, are emitted in
  lexicographic order;
- the `required` array is emitted in lexicographic member-name order;
- enum strings remain in enumerator declaration order;
- strings use normal JSON escaping; and
- nested aggregate schemas are inlined rather than referenced.

The root form is
`{"additionalProperties":false,"properties":{...},"required":[...],"type":"object"}`;
an empty aggregate uses empty `properties` and `required` values.
`std::optional<T>` is always encoded as
`{"anyOf":[<T schema>,{"type":"null"}]}` in that order.

`input_schema_v<Args>` is backed by compile-time Scry-owned storage and is
usable in constant evaluation. Its text is already in the same lexical
canonical form retained by the explicit registry; registration does not
change it.

This generated subset does not restrict explicit TOOL-001 schemas. The
explicit overload continues to accept any syntactically valid JSON object so
dynamic tools are not constrained to the reflected type system.

### Supported type matrix

`ToolArguments` is a complete, default-initializable, non-union plain
aggregate with public named data members, no base classes, and only recursively
supported member types. Static members are ignored. Bit-fields and
cv-qualified or reference data members are rejected.

The same recursive value mapping is used for input members and handler return
values:

| C++ type | JSON/schema representation | Runtime constraints |
|---|---|---|
| `bool` | `boolean` | Only a JSON boolean is accepted. |
| Non-character signed and unsigned integral types | `integer` with the C++ type's `minimum` and `maximum` | Input must be an integer and fit the exact destination range. |
| `float`, `double` | `number` | Integers and numbers are accepted when finite and representable; non-finite results are rejected. `long double` is unsupported. |
| `std::string` | `string` | Non-owning and C-string forms are unsupported. |
| Scoped enum | `string` plus `enum` of reflected enumerator names | Names are exact and case-sensitive; enumerator underlying values must be unique; unknown input names and returned values without a named enumerator are rejected. |
| `std::optional<T>` | `anyOf` containing `T` and `null` | Nullability is controlled only by the type; nested `optional` is rejected because its layers are indistinguishable in JSON. See presence rules below. |
| `std::vector<T, Allocator>` | `array` with `items` | Every `std::vector<bool, Allocator>` specialization is unsupported. The existing tool-argument/result byte bounds limit allocation. |
| `std::array<T, N>` | `array` with `items` and equal `minItems`/`maxItems` | Input length must equal `N`. |
| Nested plain aggregate | Closed `object` with recursively generated properties | Unknown and missing members follow the same rules at every depth. |

Pointers, smart pointers, `char`, `signed char`, `unsigned char`, `wchar_t`,
`char8_t`, `char16_t`, `char32_t`, byte types, C strings,
`std::string_view`, raw arrays, associative containers, sets, linked
containers, tuples, pairs, variants, spans/views, nested optionals, scoped
enums with aliased/duplicate underlying values, unions, inheritance,
polymorphic classes, and arbitrary user or Glaze serialization customizations
are outside M3. Unsupported roots, members, handlers, returns, annotations, or
traits fail at the reflected registration call with a Scry-owned concept or
`static_assert` diagnostic.

### Presence, defaults, nullability, and strict decoding

Presence and nullability are independent:

| Member declaration | May be omitted? | May be JSON `null`? | Omission result |
|---|---:|---:|---|
| `T value;` | No | No | Missing-member tool error |
| `T value = initializer;` | Yes | No | Preserve the C++ initializer |
| `std::optional<T> value;` | No | Yes | Missing-member tool error |
| `std::optional<T> value = initializer;` | Yes | Yes | Preserve the C++ initializer |

Only `std::meta::has_default_member_initializer` controls omission and
membership in `required`. Only `std::optional` controls nullability. Scry does
not emit JSON Schema's `default` keyword: a C++ default member initializer need
not be a constant expression, and its value remains C++ runtime behavior.

The generated decoder is strict and recursive. It rejects a non-object root,
unknown fields, missing required fields, wrong JSON kinds, disallowed null,
integer sign or range errors, non-finite or out-of-range floating values,
unknown enum names, and fixed-array length errors. These are handler-boundary
tool errors, not provider protocol failures, so the existing dispatcher turns
them into bounded model-visible error results and allows the model to recover.
Framework payload-limit failures retain the existing fatal `resource_limit`
behavior.

Tool arguments reach reflected handlers through Scry's canonical parsed JSON
value. That parsed value is authoritative for duplicate-key behavior:
duplicate lexical occurrences are not separately observable to M3 after the
existing JSON boundary has canonicalized the object. Unknown-field and member
count checks therefore operate on the canonical unique-key object. Rejecting
duplicate lexical keys would require a separate, global change to the M2 JSON
parser and is not part of M3.

### Parameter descriptions

When P3394 annotations are available, a member may use Scry's structural
annotation:

```cpp
struct ForecastArgs {
  [[=scry::reflection::description{"City to query"}]]
  std::string city;
};
```

The portable customization path is a specialization of
`scry::reflection::tool_traits<Args>` whose `descriptions` array contains
`parameter_description{member, text}` entries. A matching trait entry
overrides the annotation for that member; a member absent from the trait falls
back to its annotation. This makes the trait both the fallback for a P2996
toolchain without P3394 and an explicit override on the supported toolchain.

Unknown or duplicate trait member names, multiple Scry description annotations
on one member, and malformed description metadata are compile-time errors.
Unrelated annotations are ignored. A member with neither source simply omits
`description`. The same lookup rules apply recursively to nested aggregates.

### Toolchains, packaging, and dependency firewall

The stable package remains `scry::scry`, requires C++23, and has reflection
disabled. Reflection is a separate optional package component and target,
`scry::reflection`, built and installed only when explicitly enabled. A
consumer requests it with
`find_package(scry CONFIG REQUIRED COMPONENTS reflection)`, links that target,
and thereby opts into C++26 plus the required reflection flags; a core-only
consumer acquires neither the language-mode requirement nor a reflection
runtime dependency.

GCC 16 or newer with `-std=c++26 -freflection` is the supported M3 reflection
toolchain. Support is feature-probed, including the P2996 feature level, rather
than inferred only from the compiler version. P3394 descriptions are selected
when the annotation queries are present; `tool_traits` remains available
regardless. clang-p2996 is deferred to manual, non-gating compatibility work:
it is not accepted by the supported reflection configuration and must not
produce installable or release artifacts. Stable GCC/Clang reflection-OFF
builds remain authoritative for the core.

The public reflection headers contain only standard-library and Scry-owned
types. Runtime JSON parsing is reached through a Scry-owned bridge; Glaze is
included only by compiled implementation files and is a private dependency.
The installed component must compile for a downstream consumer without a
Glaze include directory or exported Glaze target.

## Verification evidence

> **Amended 2026-07 by [ADR 0011](0011-absolute-quality-gates.md):** the
> original checked exclusion validator and the 95% adjusted-decision / 100%
> function codec floors were replaced by stock gcovr
> `--fail-under-decision 85` / `--fail-under-function 95` thresholds; the
> compiled bridge's 95% CFG branch floor is unchanged, suite repetition moved
> to the TSan leg, and `scripts/ci-reflection.sh` additionally compiles the
> core-only C++23 consumer with non-reflection GCC 14 against the
> reflection-enabled installation. These mechanics are historical after the
> ADR 0012 amendment below.
>
> **Amended 2026-07 by
> [ADR 0012](0012-release-infrastructure-simplification.md):** the
> `scripts/reflection-coverage.sh` gcovr coverage leg from the prior
> verification amendments was retired at the v0.0.1 release posture. The
> build, 27-test suite, install
> audit, downstream component consumer, core-surface proof, and ASan+UBSan
> rerun in `scripts/ci-reflection.sh` remain the live gate.

The supported M3 path is live:

- `reflection_tests.cpp` provides compile-time concept and exact-schema
  assertions plus 17 runtime schema, decode, encode, typed-handler, and
  registration tests;
- `json_bridge_tests.cpp` provides five focused Scry-owned JSON bridge tests;
- five `reflection.compile-fail.*` tests require stable `ToolArguments`,
  `ToolHandlerFor`, and invalid-description diagnostics;
- `scry_header_audit` and the include-first `scry_header_reflection` target
  enforce the public boundary;
- `scripts/ci-reflection.sh` performs a fresh GCC 16/P2996-probed build, runs
  the full configured suite, including all 27 reflection-labelled tests,
  installs and audits the optional component, and builds/runs a
  downstream `find_package(scry CONFIG REQUIRED COMPONENTS reflection)`
  consumer;
- the same gate builds a core-only C++23 consumer with non-reflection GCC 14
  against the reflection-enabled installation, proving that the stable core
  surface remains severable from the experimental component;
- the same script creates a separate ASan+UBSan build and reruns all 27
  reflection-labelled tests;
- the reflection-OFF gate builds, installs, audits the absence of every
  reflection artifact, and builds/runs the stable C++23 downstream consumer.

M3 completion evidence does not claim a randomized property-generated
reflection suite, a reflection-specific fuzz target, or a manual clang-p2996
compatibility run. Those are separate future hardening or compatibility work
and must remain visibly unclaimed until their gates exist and pass.

Ordinary runtime coverage cannot observe branches executed solely during
constant evaluation. The type-directed consteval paths are covered by the
compile-time positive and negative matrix rather than represented by a
misleading runtime percentage. The instrumentable codec and compiled JSON
bridge remain covered by deterministic runtime and error-path cases, repeated
under ASan+UBSan. The gcovr floors, exclusion validator, and compiler-artifact
allowance were historical gate mechanics retired by ADR 0012; they are not
part of the live M3 completion evidence.

## Consequences

M3 remains sugar over the proven M2 boundary. Applications gain typed
registration while dynamic tools, unsupported C++ shapes, and stable
toolchains retain the explicit API.

The initial type set is intentionally smaller than Glaze's capabilities.
Expanding it is an additive requirements decision with schema, decode, encode,
diagnostic, and portability tests; it is not inherited automatically from a
dependency upgrade.

Required-but-nullable parameters are expressible (`std::optional<T>` without a
default member initializer), as are omittable-but-non-null parameters (`T`
with an initializer). This precision is slightly less conventional than
treating every optional as omittable, but it keeps C++ declaration semantics,
schema `required`, and runtime construction aligned.

The separate component and Scry-owned JSON bridge add a small amount of
plumbing. In return, core consumers never inherit experimental language flags
or third-party headers, and the reflection layer remains genuinely severable.
