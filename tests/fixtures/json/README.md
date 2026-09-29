# JSON golden fixtures

Expected results for Scry's JSON layer (`src/kernel/json/document.hpp`), checked
by `tests/kernel/json_fixture_tests.cpp` (`kernel.` tests). They pin two things
that must not drift: which inputs the parser accepts, and the exact bytes of the
canonical writer.

## Provenance

Every expectation was produced by the Glaze-backed codec the layer replaces
(`glz::generic_sorted_u64` with Scry's read and write options), and every one
was checked against the new layer as it was written. The layer departs from
Glaze deliberately in two places, both about the kind a number reads as, and
the expectations include those departures:

- A number with an exponent is a double even when its value is whole, so
  `1.0e19` and `1e19` both write as `1E19` (Glaze wrote the latter as
  `10000000000000000000`).
- `-0` is the double -0.0 and writes as `-0` (Glaze read it as the integer 0
  and wrote `0`).

Without them Glaze's canonical form is not idempotent. The differential fuzz
target `tests/fuzz/json_differential_fuzz.cpp` applies the same two rules as a
rewrite of the input Glaze sees, and asserts everything else byte for byte.

## Files

| File | Cases |
|---|---|
| `corpus.txt` | Every file under `tests/fuzz/corpus/`, whole |
| `corpus_prefixes.list` | Which prefix lengths of each of those files parse |
| `adversarial.txt` | Inputs aimed at each rule: bytes, escapes, UTF-8, numbers, keys, nesting |
| `fuzz_findings.txt` | The differential fuzzer's minimized corpus: inputs that reached new code in either implementation |

## Format

A `.txt` file is a list of cases. A case is an input line starting with `< `
(a bare `<` for empty input) followed by an expectation line: `> ` and the
canonical text, or `x` for a rejection. Lines starting with `#` name the case that follows or comment on the
file; blank lines separate cases. In input and expectation text, `%HH` is the
byte with hexadecimal value `HH`; every other character stands for itself.
Bytes outside printable ASCII, `%` itself, and trailing spaces are always
written as `%HH`, so each case is one line and the files are plain ASCII.

A line of `corpus_prefixes.list` is a corpus path relative to
`tests/fuzz/corpus/`, a space, and the accepted prefix lengths as
comma-separated lengths and inclusive `low-high` ranges, or `none`. Every other
length from zero to the file's size must be rejected. Adding a corpus file means
adding its line here.
