# Third-party notices

## Distributed in the scry package

### Glaze

Scry's internal JSON handling uses [Glaze](https://github.com/stephenberry/glaze)
(pinned by commit via CMake FetchContent). Glaze is header-only, so its code
is compiled into the installed `libscry.a`; no Glaze header, type, or CMake
target appears in Scry's public surface.

```text
MIT License

Copyright (c) 2019 - present, Stephen Berry

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Linked, not distributed

- [libcurl](https://curl.se/libcurl/) (curl license) — found on the host
  system via `find_package(CURL)`; never vendored.

## Development and example dependencies (not in the installed package)

- [Catch2](https://github.com/catchorg/Catch2) (BSL-1.0) — test framework,
  fetched only when `SCRY_BUILD_TESTS=ON`.
- [Dear ImGui](https://github.com/ocornut/imgui) (MIT) — fetched only for the
  opt-in `SCRY_BUILD_IMGUI_SHOWCASE=ON` example; the package-absence audit in
  `scripts/ci-showcase.sh` proves it never enters the installed package.
