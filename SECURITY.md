# Security Policy

## Supported versions

Only the latest 0.x release receives security fixes.

## Reporting a vulnerability

Report suspected vulnerabilities privately through GitHub's private
vulnerability reporting on this repository (**Security** tab → **Report a
vulnerability**). Please do not open a public issue for a suspected
vulnerability. You should receive an initial response within a week.

Scry is an HTTP/TLS client library that parses attacker-adjacent input: a
compromised or buggy server must not be able to crash or subvert the host
application. Reports about TLS validation, SSE parsing, HTTP header handling,
provider response decoding, or memory safety on those paths are all in scope.
