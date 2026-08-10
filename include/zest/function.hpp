/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace zest
{

/**
 * A non-owning reference to any callable, for callbacks that do not outlive the
 * call that installs them.
 *
 * Two pointers wide, never allocates, and --- unlike a bare `void (*)(void *)`
 * plus a context argument --- accepts a lambda that captures. That pattern is
 * what forces application code back into writing static trampolines and casting
 * context pointers by hand, which is precisely the boilerplate this library
 * exists to remove.
 *
 * The referenced callable must outlive the `FunctionRef`. Use `InplaceFunction`
 * when the callback has to be stored.
 */
template <typename Signature> class FunctionRef;

namespace detail
{

/*
 * A callable object is erased through `void *`, but a function pointer cannot
 * legally round-trip through one, so the two are kept in a union and the thunk
 * knows which arm it installed.
 */
template <typename R, typename... Args> union ErasedCallable {
	void *object;
	R (*function)(Args...);
};

template <typename R, typename... Args> union ErasedNothrowCallable {
	void *object;
	R (*function)(Args...) noexcept;
};

template <typename F>
concept NotFunction = !std::is_function_v<std::remove_reference_t<F>>;

} /* namespace detail */

template <typename R, typename... Args> class FunctionRef<R(Args...)>
{
      public:
	FunctionRef() = delete;

	/** Refer to a callable object. It must outlive this reference. */
	template <typename F>
		requires(!std::is_same_v<std::decay_t<F>, FunctionRef> && detail::NotFunction<F> &&
			 std::is_invocable_r_v<R, F &, Args...>)
	constexpr FunctionRef(F &&callable) noexcept
		: target_{.object = const_cast<void *>(
				  static_cast<const void *>(std::addressof(callable)))},
		  invoke_{[](Target target, Args... args) -> R {
			  return static_cast<R>((*static_cast<std::remove_reference_t<F> *>(
				  target.object))(std::forward<Args>(args)...));
		  }}
	{
	}

	/** Refer to a plain function. */
	constexpr FunctionRef(R (*function)(Args...)) noexcept
		: target_{.function = function}, invoke_{[](Target target, Args... args) -> R {
			  return target.function(std::forward<Args>(args)...);
		  }}
	{
	}

	constexpr R operator()(Args... args) const
	{
		return invoke_(target_, std::forward<Args>(args)...);
	}

      private:
	using Target = detail::ErasedCallable<R, Args...>;

	Target target_;
	R (*invoke_)(Target, Args...);
};

template <typename R, typename... Args> class FunctionRef<R(Args...) noexcept>
{
      public:
	FunctionRef() = delete;

	/** Refer to a callable object. It must outlive this reference. */
	template <typename F>
		requires(!std::is_same_v<std::decay_t<F>, FunctionRef> && detail::NotFunction<F> &&
			 std::is_nothrow_invocable_r_v<R, F &, Args...>)
	constexpr FunctionRef(F &&callable) noexcept
		: target_{.object = const_cast<void *>(
				  static_cast<const void *>(std::addressof(callable)))},
		  invoke_{[](Target target, Args... args) noexcept -> R {
			  return static_cast<R>((*static_cast<std::remove_reference_t<F> *>(
				  target.object))(std::forward<Args>(args)...));
		  }}
	{
	}

	/** Refer to a plain function. */
	constexpr FunctionRef(R (*function)(Args...) noexcept) noexcept
		: target_{.function = function},
		  invoke_{[](Target target, Args... args) noexcept -> R {
			  return target.function(std::forward<Args>(args)...);
		  }}
	{
	}

	constexpr R operator()(Args... args) const noexcept
	{
		return invoke_(target_, std::forward<Args>(args)...);
	}

      private:
	using Target = detail::ErasedNothrowCallable<R, Args...>;

	Target target_;
	R (*invoke_)(Target, Args...) noexcept;
};

/**
 * An owning callable with fixed inline storage and no allocation.
 *
 * Holds any callable whose object fits in `Capacity` bytes, so a lambda
 * capturing `this` and a handle or two can be stored in a member, a work item, or
 * an event table without a heap. Assigning a callable that does not fit is a
 * compile error naming the size required, rather than a silent allocation.
 *
 * Not thread safe: an instance must not be reassigned while another context may
 * be invoking it.
 */
template <typename Signature, std::size_t Capacity = 2 * sizeof(void *)> class InplaceFunction;

template <typename R, typename... Args, std::size_t Capacity>
class InplaceFunction<R(Args...) noexcept, Capacity>
{
      public:
	static constexpr std::size_t capacity = Capacity;

	constexpr InplaceFunction() noexcept = default;

	template <typename F>
		requires(!std::is_same_v<std::decay_t<F>, InplaceFunction> &&
			 std::is_nothrow_invocable_r_v<R, std::decay_t<F> &, Args...>)
	InplaceFunction(F &&callable) noexcept
	{
		assign(std::forward<F>(callable));
	}

	InplaceFunction(const InplaceFunction &) = delete;
	InplaceFunction &operator=(const InplaceFunction &) = delete;

	InplaceFunction(InplaceFunction &&other) noexcept
	{
		if (other.operations_ != nullptr) {
			other.operations_->move(other.storage_, storage_);
			operations_ = other.operations_;
			other.reset();
		}
	}

	InplaceFunction &operator=(InplaceFunction &&other) noexcept
	{
		if (this != &other) {
			reset();
			if (other.operations_ != nullptr) {
				other.operations_->move(other.storage_, storage_);
				operations_ = other.operations_;
				other.reset();
			}
		}
		return *this;
	}

	~InplaceFunction() noexcept
	{
		reset();
	}

	template <typename F>
		requires(!std::is_same_v<std::decay_t<F>, InplaceFunction> &&
			 std::is_nothrow_invocable_r_v<R, std::decay_t<F> &, Args...>)
	InplaceFunction &operator=(F &&callable) noexcept
	{
		reset();
		assign(std::forward<F>(callable));
		return *this;
	}

	/** Invoke the stored callable. Calling an empty instance is undefined. */
	R operator()(Args... args) const noexcept
	{
		return operations_->invoke(const_cast<std::byte *>(storage_),
					   std::forward<Args>(args)...);
	}

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return operations_ != nullptr;
	}

	/** Destroy any stored callable, leaving the instance empty. */
	void reset() noexcept
	{
		if (operations_ != nullptr) {
			operations_->destroy(storage_);
			operations_ = nullptr;
		}
	}

      private:
	struct Operations {
		R (*invoke)(std::byte *, Args...) noexcept;
		void (*destroy)(std::byte *) noexcept;
		void (*move)(std::byte *from, std::byte *to) noexcept;
	};

	template <typename F> void assign(F &&callable) noexcept
	{
		using Callable = std::decay_t<F>;
		static_assert(sizeof(Callable) <= Capacity,
			      "callable does not fit; raise the InplaceFunction capacity");
		static_assert(alignof(Callable) <= alignof(std::max_align_t),
			      "callable has stricter alignment than the inline storage provides");
		static_assert(std::is_nothrow_move_constructible_v<Callable>,
			      "a stored callable must be nothrow move constructible");

		::new (static_cast<void *>(storage_)) Callable{std::forward<F>(callable)};

		static constexpr Operations operations{
			.invoke = [](std::byte *storage, Args... args) noexcept -> R {
				return static_cast<R>((*std::launder(reinterpret_cast<Callable *>(
					storage)))(std::forward<Args>(args)...));
			},
			.destroy =
				[](std::byte *storage) noexcept {
					std::launder(reinterpret_cast<Callable *>(storage))
						->~Callable();
				},
			.move =
				[](std::byte *from, std::byte *to) noexcept {
					auto *source =
						std::launder(reinterpret_cast<Callable *>(from));
					::new (static_cast<void *>(to))
						Callable{std::move(*source)};
					source->~Callable();
				},
		};
		operations_ = &operations;
	}

	alignas(std::max_align_t) std::byte storage_[Capacity]{};
	const Operations *operations_{nullptr};
};

} /* namespace zest */
