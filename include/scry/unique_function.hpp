#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace scry {

template <typename Signature> class UniqueFunction;

template <typename Return, typename... Args>
class UniqueFunction<Return(Args...)> final {
public:
  UniqueFunction() noexcept = default;
  UniqueFunction(std::nullptr_t) noexcept {}

  template <typename Callable>
    requires(!std::same_as<std::remove_cvref_t<Callable>, UniqueFunction> &&
             std::is_invocable_r_v<Return, std::decay_t<Callable>&, Args...>)
  UniqueFunction(Callable&& callable)
      : object_(new std::decay_t<Callable>(std::forward<Callable>(callable))),
        invoke_(&invoke<Callable>), destroy_(&destroy<Callable>) {}

  ~UniqueFunction() { reset(); }

  UniqueFunction(UniqueFunction&& other) noexcept
      : object_(std::exchange(other.object_, nullptr)),
        invoke_(std::exchange(other.invoke_, nullptr)),
        destroy_(std::exchange(other.destroy_, nullptr)) {}

  UniqueFunction& operator=(UniqueFunction&& other) noexcept {
    if (this != &other) {
      reset();
      object_ = std::exchange(other.object_, nullptr);
      invoke_ = std::exchange(other.invoke_, nullptr);
      destroy_ = std::exchange(other.destroy_, nullptr);
    }
    return *this;
  }

  UniqueFunction(const UniqueFunction&) = delete;
  UniqueFunction& operator=(const UniqueFunction&) = delete;

  [[nodiscard]] explicit operator bool() const noexcept { return object_ != nullptr; }

  Return operator()(Args... args) {
    if (object_ == nullptr) {
      throw std::bad_function_call{};
    }
    return invoke_(object_, std::forward<Args>(args)...);
  }

  void reset() noexcept {
    if (object_ != nullptr) {
      destroy_(object_);
      object_ = nullptr;
      invoke_ = nullptr;
      destroy_ = nullptr;
    }
  }

private:
  template <typename Callable> static Return invoke(void* object, Args&&... args) {
    using StoredCallable = std::decay_t<Callable>;
    return std::invoke(*static_cast<StoredCallable*>(object),
                       std::forward<Args>(args)...);
  }

  template <typename Callable> static void destroy(void* object) noexcept {
    using StoredCallable = std::decay_t<Callable>;
    delete static_cast<StoredCallable*>(object);
  }

  void* object_{};
  Return (*invoke_)(void*, Args&&...){};
  void (*destroy_)(void*) noexcept {};
};

} // namespace scry
