# JSON golden fixtures

These files hold the expected results for the JSON layer of Scry
(`src/kernel/json/document.hpp`). `tests/kernel/json_fixture_tests.cpp` checks
them. They pin which inputs the parser accepts. They also pin the exact bytes
that the canonical writer writes.

## Provenance

Scry used a third-party header-only codec through v0.5 (see `docs/releases/`).
That codec made each expectation before the JSON layer of Scry replaced it. We
checked each expectation against the new layer when we wrote it. Before we
removed the old codec, a differential fuzz target compared the two
implementations for 45 minutes and 4.3 million executions. It found no
divergence.

The layer is intentionally different from the old codec in two places. Both
places are about the kind that a number reads as. The expectations include
these differences:

- A number with an exponent is a double, also when its value is whole. Thus,
  `1.0e19` and `1e19` both write as `1E19`. The old codec wrote the second
  number as `10000000000000000000`.
- `-0` is the double -0.0, and it writes as `-0`. The old codec read it as the
  integer 0 and wrote `0`.

Without these differences, the old canonical form was not idempotent.

## Files

The case files are generated, frozen text of approximately 16,000 lines. Git
holds them as one archive, `goldens.tar.xz`. At configure time, the test build
unpacks this archive into `build/<preset>/tests/fixtures/json/`.

| File in the archive | Cases |
|---|---|
| `corpus.txt` | Every file under `tests/fuzz/corpus/`, whole |
| `corpus_prefixes.list` | Which prefix lengths of each of those files parse |
| `adversarial.txt` | Inputs aimed at each rule: bytes, escapes, UTF-8, numbers, keys, nesting |
| `fuzz_findings.txt` | The differential fuzzer's minimized corpus: inputs that reached new code in either implementation |

## Format

A `.txt` file is a list of cases. Each case has an input line and then an
expectation line:

- The input line starts with `< `. For empty input, the line is a bare `<`.
- The expectation line is `> ` and the canonical text. For a rejection, the line
  is `x`.

A line that starts with `#` gives the name of the next case, or it is a comment
on the file. Blank lines separate cases.

In input and expectation text, `%HH` is the byte with the hexadecimal value
`HH`. All other characters are literal. The files always write these as `%HH`:

- bytes that are not printable ASCII
- `%` itself
- spaces at the end of a line.

Thus, each case is one line, and the files are plain ASCII.

A line of `corpus_prefixes.list` has three parts:

1. A corpus path, relative to `tests/fuzz/corpus/`.
2. A space.
3. The accepted prefix lengths, or `none`. The list separates lengths and
   inclusive `low-high` ranges with commas.

The parser must reject each other length from zero to the size of the file.

## Adding a case or a corpus file

The old codec is removed, so a new expectation comes from the layer itself. The
rules in `src/kernel/json/document.hpp` must justify each new expectation. Do
not only record the output of the layer.

For a new file under `tests/fuzz/corpus/`, do these steps:

1. Add its line to `corpus_prefixes.list`. The prefix tests fail until this
   line is present.
2. If the file must be pinned whole, add a case to `corpus.txt`.

To edit the files, unpack the archive here and change the files. Then repack the
archive reproducibly, and delete the unpacked copies:

```sh
cd tests/fixtures/json
tar -xJf goldens.tar.xz
# edit, then:
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 --format=gnu \
  -cf - adversarial.txt corpus.txt corpus_prefixes.list fuzz_findings.txt |
  xz -9e > goldens.tar.xz
rm adversarial.txt corpus.txt corpus_prefixes.list fuzz_findings.txt
```
