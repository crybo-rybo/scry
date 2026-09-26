# Sourced by the scripts that bound a run with GNU timeout. macOS ships no
# `timeout`; coreutils installs it as `gtimeout` unless gnubin is on PATH.
gnu_timeout() {
  if command -v timeout >/dev/null 2>&1; then
    command timeout "$@"
  elif command -v gtimeout >/dev/null 2>&1; then
    command gtimeout "$@"
  else
    echo "GNU timeout is required (install coreutils on macOS)." >&2
    return 2
  fi
}
