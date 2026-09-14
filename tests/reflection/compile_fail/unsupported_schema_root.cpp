#include <scry/reflection.hpp>

// Raw JSON is outside the closed SupportedValue matrix, so it cannot be a schema root.
static_assert(!scry::reflection::schema_v<scry::Json>.empty());
