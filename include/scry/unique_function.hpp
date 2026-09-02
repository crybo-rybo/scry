#pragma once

/**
 * @file unique_function.hpp
 * @brief Minimal move-only owning type erasure for callbacks and tool handlers.
 *
 * Supported C++23 standard libraries do not all provide a conforming
 * `std::move_only_function`. UniqueFunction supplies only the behavior Scry's callable
 * boundaries need: ownership of move-only captures, empty-state inspection, invocation,
 * move transfer, and reset.
 */

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
/// standard libraries provide a conforming `std::move_only_function`. Only ordinary
/// function signatures are specialized; cv/ref/noexcept-qualified signatures are not
/// part of this deliberately small facility.
/// @tparam Signature Function signature used for invocation.
template <typename Signature> class UniqueFunction;

/// Move-only owning callable wrapper for `Return(Args...)`.
///
/// The callable is allocated once and invoked as a non-const lvalue, which supports
/// mutable lambdas and move-only state. UniqueFunction performs no synchronization;
/// concurrent access to the same wrapper is the caller's responsibility.
/// @tparam Return Required invocation result type.
/// @tparam Args Erased function argument types.
template <typename Return, typename... Args>
class UniqueFunction<Return(Args...)> final {
public:
  /// Constructs an empty callable.
  UniqueFunction() noexcept = default;

  /// Constructs an empty callable from `nullptr`, enabling nullable aggregate members.
  UniqueFunction(std::nullptr_t) noexcept {}

  /// Takes ownership of an invocable object, including one with move-only captures.
  ///
  /// The decayed callable type must be invocable as an lvalue with Args... and produce
  /// a result compatible with Return. Construction may propagate allocation or callable
  /// move/copy-construction exceptions.
  /// @tparam Callable Callable object type inferred from the argument.
  /// @param callable Object to own and invoke.
  template <typename Callable>
    requires(!std::same_as<std::remove_cvref_t<Callable>, UniqueFunction> &&
             std::is_invocable_r_v<Return, std::decay_t<Callable>&, Args...>)
  UniqueFunction(Callable&& callable)
      : object_(new std::decay_t<Callable>(std::forward<Callable>(callable))),
        invoke_(&invoke<Callable>), destroy_(&destroy<Callable>) {}

  /// Destroys the owned callable, if any, without invoking it.
  ~UniqueFunction() { reset(); }

  /// Moves ownership from another wrapper.
  /// @param other Wrapper whose callable is transferred.
  /// @post `other` is empty.
  UniqueFunction(UniqueFunction&& other) noexcept
      : object_(std::exchange(other.object_, nullptr)),
        invoke_(std::exchange(other.invoke_, nullptr)),
        destroy_(std::exchange(other.destroy_, nullptr)) {}

  /// Replaces the owned callable with one moved from another wrapper.
  ///
  /// Self-move assignment leaves the wrapper unchanged.
  /// @param other Wrapper whose callable is transferred.
  /// @return This wrapper.
  /// @post For distinct objects, `other` is empty.
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
  /// The erased signature owns its by-value arguments exactly like `std::function`;
  /// reference arguments retain their reference semantics. Exceptions from the target
  /// propagate unchanged to the caller.
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
  ///
  /// Calling reset() on an empty wrapper is a no-op.
  void reset() noexcept {
    if (object_ != nullptr) {
      destroy_(object_);
      object_ = nullptr;
      invoke_ = nullptr;
      destroy_ = nullptr;
    }
  }

private:
  /// Casts erased storage back to its concrete callable and invokes it.
  /// @tparam Callable Concrete type stored in object.
  /// @param object Non-null pointer to the stored Callable.
  /// @param args Signature arguments forwarded to the callable.
  /// @return Callable result when Return is non-void.
  template <typename Callable> static Return invoke(void* object, Args&&... args) {
    using StoredCallable = std::decay_t<Callable>;
    return std::invoke(*static_cast<StoredCallable*>(object),
                       std::forward<Args>(args)...);
  }

  /// Deletes an erased callable using its concrete allocation type.
  /// @tparam Callable Concrete type stored in object.
  /// @param object Non-null pointer created by the converting constructor.
  template <typename Callable> static void destroy(void* object) noexcept {
    using StoredCallable = std::decay_t<Callable>;
    delete static_cast<StoredCallable*>(object);
  }

  /// Owned type-erased callable allocation, or null when empty.
  void* object_{};
  /// Type-specific invocation trampoline matching the erased signature.
  Return (*invoke_)(void*, Args&&...){};
  /// Type-specific destruction trampoline for object_.
  void (*destroy_)(void*) noexcept {};
};

} // namespace scry
