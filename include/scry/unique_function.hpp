#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace scry {

/// Move-only owning callable wrapper specialized by function signature.
///
/// UniqueFunction is Scry's public callback and handler boundary until all supported
/// standard libraries provide a conforming `std::move_only_function`.
template <typename Signature> class UniqueFunction;

/// Move-only owning callable wrapper for `Return(Args...)`.
template <typename Return, typename... Args>
class UniqueFunction<Return(Args...)> final {
public:
  /// Constructs an empty callable.
  UniqueFunction() noexcept = default;

  /// Constructs an empty callable from `nullptr`.
  UniqueFunction(std::nullptr_t) noexcept {}

  /// Takes ownership of an invocable object, including one with move-only captures.
  ///
  /// A null function pointer or null member pointer produces an empty wrapper. A
  /// callable whose return type differs from `Return` is accepted when it is
  /// implicitly convertible, and any return is discarded when `Return` is void.
  /// @tparam Callable Callable object type inferred from the argument.
  /// @param callable Object to own and invoke.
  template <typename Callable>
    requires(!std::same_as<std::remove_cvref_t<Callable>, UniqueFunction> &&
             std::is_invocable_r_v<Return, std::decay_t<Callable>&, Args...>)
  UniqueFunction(Callable&& callable) {
    using StoredCallable = std::decay_t<Callable>;
    if constexpr (std::is_pointer_v<StoredCallable> ||
                  std::is_member_pointer_v<StoredCallable>) {
      if (callable == nullptr) {
        return;
      }
    }
    object_ = new StoredCallable(std::forward<Callable>(callable));
    invoke_ = &invoke<Callable>;
    destroy_ = &destroy<Callable>;
  }

  /// Destroys the owned callable, if any.
  ~UniqueFunction() { reset(); }

  /// Moves ownership from another wrapper.
  /// @param other Wrapper whose callable is transferred.
  UniqueFunction(UniqueFunction&& other) noexcept
      : object_(std::exchange(other.object_, nullptr)),
        invoke_(std::exchange(other.invoke_, nullptr)),
        destroy_(std::exchange(other.destroy_, nullptr)) {}

  /// Replaces the owned callable with one moved from another wrapper.
  /// @param other Wrapper whose callable is transferred.
  /// @return This wrapper.
  UniqueFunction& operator=(UniqueFunction&& other) noexcept {
    if (this != &other) {
      reset();
      object_ = std::exchange(other.object_, nullptr);
      invoke_ = std::exchange(other.invoke_, nullptr);
      destroy_ = std::exchange(other.destroy_, nullptr);
    }
    return *this;
  }

  /// Callable wrappers are not copyable.
  UniqueFunction(const UniqueFunction&) = delete;

  /// Callable wrappers are not copy-assignable.
  UniqueFunction& operator=(const UniqueFunction&) = delete;

  /// Reports whether this wrapper owns a callable.
  /// @return true when the wrapper is nonempty.
  [[nodiscard]] explicit operator bool() const noexcept { return object_ != nullptr; }

  /// Invokes the owned callable.
  ///
  /// The erased signature owns its by-value arguments exactly like `std::function`.
  /// @param args Arguments forwarded to the callable.
  /// @return The callable's result when Return is non-void.
  /// @throws std::bad_function_call if this wrapper is empty.
  Return operator()(Args... args) { // NOLINT(performance-unnecessary-value-param)
    if (object_ == nullptr) {
      throw std::bad_function_call{};
    }
    return invoke_(object_, std::forward<Args>(args)...);
  }

  /// Destroys the owned callable and makes this wrapper empty.
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
    return std::invoke_r<Return>(*static_cast<StoredCallable*>(object),
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
