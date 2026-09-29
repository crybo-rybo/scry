#!/bin/sh
# Doxygen input filter. Doxygen 1.9 does not parse a P3394 annotation between the
# class-key and the class name (`struct [[= scry::reflection::tag{"text"}]] T`)
# and drops the class, so the documentation build removes that annotation before
# Doxygen reads a header. Nothing else in the text changes.
exec sed -E 's/^([[:space:]]*(struct|class))[[:space:]]*\[\[=[^]]*\]\][[:space:]]*/\1 /' "$1"
