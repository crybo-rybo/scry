#pragma once

#include "core/backend.hpp"

#include <memory>
#include <scry/config.hpp>

namespace scry::detail {

// The llamad backend: ProviderDialect::llamad, one server-streaming Chat call
// per attempt to a local llamad daemon over gRPC on a Unix socket. The channel
// is created here, once per Harness, from the validated Config's base_url; no
// gRPC type crosses this header. Defined only when Scry is built with
// SCRY_WITH_LLAMAD.
[[nodiscard]] std::unique_ptr<ModelBackend> make_llamad_backend(const Config& config);

} // namespace scry::detail
