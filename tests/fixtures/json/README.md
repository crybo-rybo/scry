# JSON golden fixtures

Expected results for Scry's JSON layer (`src/kernel/json/document.hpp`). They
pin two things that must not drift: which inputs the parser accepts, and the
exact bytes of the canonical writer. `tests/kernel/json_fixture_tests.cpp`
checks the layer itself against them, and
`tests/runtime/json_codec_validation_tests.cpp` checks the codec entry points
and `JsonView` above it.

## Provenance

Every expectation was produced by the third-party header-only codec that Scry
used through v0.5 (see `docs/releases/`), before its own layer replaced it, and
every one was checked against the new layer as it was written. A differential
fuzz target held the two together for 45 minutes and 4.3 million executions
without a divergence before the old codec was removed. The layer departs from
the old codec deliberately in two places, both about the kind a number reads
as, and the expectations include those departures:

- A number with an exponent is a double even when its value is whole, so
  `1.0e19` and `1e19` both write as `1E19` (the old codec wrote the latter as
  `10000000000000000000`).
- `-0` is the double -0.0 and writes as `-0` (the old codec read it as the
  integer 0 and wrote `0`).

Without them the old canonical form was not idempotent.

## Files

The case files are generated, frozen text (about 16,000 lines), so they are
checked in as one archive, `goldens.tar.xz`, which the test build unpacks into
`build/<preset>/tests/fixtures/json/` at configure time.

| File in the archive | Cases |
|---|---|
| `corpus.txt` | Every file under `tests/fuzz/corpus/`, whole |
| `corpus_prefixes.list` | Which prefix lengths of each of those files parse |
| `adversarial.txt` | Inputs aimed at each rule: bytes, escapes, UTF-8, numbers, keys, nesting |
| `fuzz_findings.txt` | The differential fuzzer's minimized corpus: inputs that reached new code in either implementation |

## Format

A `.txt` file is a list of cases. A case is an input line starting with `< `
(a bare `<` for empty input) followed by an expectation line: `> ` and the
canonical text, or `x` for a rejection. Lines starting with `#` name the case
that follows or comment on the file; blank lines separate cases. In input and
expectation text, `%HH` is the byte with hexadecimal value `HH`; every other
character stands for itself. Bytes outside printable ASCII, `%` itself, and
trailing spaces are always written as `%HH`, so each case is one line and the
files are plain ASCII.

A line of `corpus_prefixes.list` is a corpus path relative to
`tests/fuzz/corpus/`, a space, and the accepted prefix lengths as
comma-separated lengths and inclusive `low-high` ranges, or `none`. Every other
length from zero to the file's size must be rejected.

## Adding a case or a corpus file

The old codec is gone, so a new expectation comes from the layer itself and
must be justified by the rules in `src/kernel/json/document.hpp`, not merely
recorded. A new file under `tests/fuzz/corpus/` needs its line in
`corpus_prefixes.list` (the prefix tests fail until it has one) and, if it should
be pinned whole, a case in `corpus.txt`.

To edit the files, unpack the archive here, change them, and repack it
reproducibly, then delete the unpacked copies:

```sh
cd tests/fixtures/json
tar -xJf goldens.tar.xz
# edit, then:
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 --format=gnu \
  -cf - adversarial.txt corpus.txt corpus_prefixes.list fuzz_findings.txt |
  xz -9e > goldens.tar.xz
rm adversarial.txt corpus.txt corpus_prefixes.list fuzz_findings.txt
```
