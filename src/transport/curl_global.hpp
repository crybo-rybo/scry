#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <scry/error.hpp>

namespace scry::detail {

struct CurlRuntimeCapabilities {
  std::uint32_t version_number{};
  bool thread_safe{};
  bool asynchronous_dns{};
};

[[nodiscard]] Status
validate_curl_runtime_capabilities(CurlRuntimeCapabilities capabilities);

class CurlGlobalState;

class CurlGlobalLease final {
public:
  CurlGlobalLease();
  ~CurlGlobalLease();

  CurlGlobalLease(CurlGlobalLease&&) noexcept;
  CurlGlobalLease& operator=(CurlGlobalLease&&) noexcept;

  CurlGlobalLease(const CurlGlobalLease&) = delete;
  CurlGlobalLease& operator=(const CurlGlobalLease&) = delete;

  [[nodiscard]] const std::optional<Error>& error() const noexcept;

private:
  std::shared_ptr<CurlGlobalState> state_;
};

} // namespace scry::detail
