#pragma once

#include "core/provider.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <scry/config.hpp>
#include <string>
#include <vector>

namespace scry::bench {

struct ScenarioResult {
  std::uint64_t digest{};
  std::uint64_t input_bytes{};
  std::uint64_t output_bytes{};
  std::uint64_t items{};
  bool valid{};
};

class SseScenario final {
public:
  SseScenario(std::size_t chunk_bytes, bool use_crlf);

  [[nodiscard]] ScenarioResult run(bool validate);

private:
  std::string input_{};
  std::uint64_t expected_digest_{};
  std::size_t chunk_bytes_{};
  std::size_t event_count_{};
  std::size_t output_bytes_{};
  ScenarioResult oracle_{};
};

enum class OpenAiStreamShape : std::uint8_t {
  text,
  tools,
};

class OpenAiStreamScenario final {
public:
  OpenAiStreamScenario(OpenAiStreamShape shape, std::size_t scale);

  [[nodiscard]] ScenarioResult run(bool validate);

private:
  std::unique_ptr<detail::ProviderAdapter> adapter_{};
  std::vector<std::string> chunks_{};
  std::string expected_text_{};
  std::size_t expected_tool_count_{};
  std::size_t expected_fragment_count_{};
  std::size_t input_bytes_{};
  ScenarioResult oracle_{};
};

enum class RequestDialect : std::uint8_t {
  openai,
  anthropic,
};

class RequestEncodingScenario final {
public:
  RequestEncodingScenario(RequestDialect dialect, std::size_t message_count,
                          std::size_t schema_count);

  [[nodiscard]] ScenarioResult run(bool validate);

private:
  std::unique_ptr<detail::ProviderAdapter> adapter_{};
  Config config_{};
  detail::ModelRequest request_{};
  std::size_t input_bytes_{};
  ScenarioResult oracle_{};
};

class TurnMachineOperation final {
public:
  ~TurnMachineOperation();
  TurnMachineOperation(TurnMachineOperation&&) noexcept;
  TurnMachineOperation& operator=(TurnMachineOperation&&) noexcept;

  TurnMachineOperation(const TurnMachineOperation&) = delete;
  TurnMachineOperation& operator=(const TurnMachineOperation&) = delete;

  [[nodiscard]] ScenarioResult run();

private:
  struct Impl;
  explicit TurnMachineOperation(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_{};

  friend class TurnMachineScenario;
};

class TurnMachineScenario final {
public:
  TurnMachineScenario(std::size_t tool_count, std::size_t argument_bytes);

  [[nodiscard]] ScenarioResult validate();
  [[nodiscard]] TurnMachineOperation prepare() const;

private:
  [[nodiscard]] TurnMachineOperation make_operation(bool validate) const;

  std::vector<detail::ContentBlock> calls_{};
  std::vector<detail::ToolResultBlock> results_{};
  std::size_t input_bytes_{};
  ScenarioResult oracle_{};
};

enum class PumpShape : std::uint8_t {
  text_delivery,
  completion_commit,
};

class PumpOperation final {
public:
  ~PumpOperation();
  PumpOperation(PumpOperation&&) noexcept;
  PumpOperation& operator=(PumpOperation&&) noexcept;

  PumpOperation(const PumpOperation&) = delete;
  PumpOperation& operator=(const PumpOperation&) = delete;

  [[nodiscard]] ScenarioResult run();

private:
  struct Impl;
  explicit PumpOperation(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_{};

  friend class PumpScenario;
};

class PumpScenario final {
public:
  PumpScenario(PumpShape shape, std::size_t route_count);

  [[nodiscard]] ScenarioResult validate();
  [[nodiscard]] PumpOperation prepare() const;

private:
  [[nodiscard]] PumpOperation make_operation(bool validate) const;

  PumpShape shape_{};
  std::size_t route_count_{};
  ScenarioResult oracle_{};
};

enum class AdmissionShape : std::uint8_t {
  history,
  schemas,
};

class AdmissionOperation final {
public:
  ~AdmissionOperation();
  AdmissionOperation(AdmissionOperation&&) noexcept;
  AdmissionOperation& operator=(AdmissionOperation&&) noexcept;

  AdmissionOperation(const AdmissionOperation&) = delete;
  AdmissionOperation& operator=(const AdmissionOperation&) = delete;

  [[nodiscard]] ScenarioResult run();

private:
  struct Impl;
  explicit AdmissionOperation(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_{};

  friend class AdmissionScenario;
};

class AdmissionScenario final {
public:
  AdmissionScenario(AdmissionShape shape, std::size_t element_count,
                    std::size_t element_bytes);

  [[nodiscard]] ScenarioResult validate();
  [[nodiscard]] AdmissionOperation prepare() const;

private:
  [[nodiscard]] AdmissionOperation make_operation(bool validate) const;

  AdmissionShape shape_{};
  std::size_t element_count_{};
  std::size_t element_bytes_{};
  ScenarioResult oracle_{};
};

class ToolRegistryOperation final {
public:
  ~ToolRegistryOperation();
  ToolRegistryOperation(ToolRegistryOperation&&) noexcept;
  ToolRegistryOperation& operator=(ToolRegistryOperation&&) noexcept;

  ToolRegistryOperation(const ToolRegistryOperation&) = delete;
  ToolRegistryOperation& operator=(const ToolRegistryOperation&) = delete;

  [[nodiscard]] ScenarioResult run();

private:
  struct Impl;
  explicit ToolRegistryOperation(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_{};

  friend class ToolRegistryScenario;
};

class ToolRegistryScenario final {
public:
  explicit ToolRegistryScenario(std::size_t schema_count);

  [[nodiscard]] ScenarioResult validate();
  [[nodiscard]] ToolRegistryOperation prepare() const;

private:
  [[nodiscard]] ToolRegistryOperation make_operation(bool validate) const;

  std::vector<detail::ToolSchema> schemas_{};
  std::size_t input_bytes_{};
  ScenarioResult oracle_{};
};

} // namespace scry::bench
