#pragma once

#include "core/model.hpp"
#include "core/provider.hpp"

#include <atomic>
#include <scry/config.hpp>
#include <scry/error.hpp>
#include <scry/unique_function.hpp>
#include <stop_token>

namespace scry::detail {

// Receives an attempt's streamed events, in order, while the attempt runs. The
// worker supplies it; a failed Status aborts the attempt.
using ProviderEventSink = UniqueFunction<Status(ProviderEvent)>;

// The worker's only seam to a model: one attempt of one model request, whatever
// the wire. The worker keeps retries, jitter, redaction, the turn machine, and
// publication; a backend only encodes, transfers, and decodes.
//
// perform() streams ProviderSemanticOutput and ProviderTextDelta events through
// `sink` as they decode. ProviderSemanticOutput is sent at most once per
// attempt, the moment the backend first consumes text or tool-call content and
// before any text delta, because it ends the attempt's retry eligibility.
// ProviderCompleted never goes through the sink: the completed response is the
// return value, and a sink that receives one fails the attempt with protocol.
// A failed sink Status aborts the attempt and is returned as its Error.
//
// Returns the completed ModelResponse, with provider_request_id filled when the
// backend knows one, or the attempt's Error. It must return promptly once
// `shutdown` is requested or `cancelled` is set, with an ErrorCategory::cancelled
// error when either ended the attempt. It is called from the worker thread only,
// one attempt at a time, and `request` outlives the call.
class ModelBackend {
public:
  virtual ~ModelBackend() = default;

  [[nodiscard]] virtual Result<ModelResponse>
  perform(const Config& config, const ModelRequest& request, std::stop_token shutdown,
          const std::atomic<bool>& cancelled, ProviderEventSink& sink) = 0;
};

} // namespace scry::detail
