#include <scrydantic/codec.hpp>

// Raw JSON is outside the closed SupportedValue matrix, so it cannot be a schema root.
static_assert(!scrydantic::schema_v<scrydantic::Json>.empty());
