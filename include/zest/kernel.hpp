#pragma once

#include <zephyr/kernel.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <type_traits>

namespace zest
{

namespace detail
{
template <typename Rep, typename Period>
[[nodiscard]] constexpr k_timeout_t timeout(std::chrono::duration<Rep, Period> value) noexcept
{
	if (value == std::chrono::duration<Rep, Period>::max()) {
		return K_FOREVER;
	}
	const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(value);
	return K_MSEC(milliseconds.count());
}
} /* namespace detail */

/** A fixed-capacity, allocation-free Zephyr message queue. */
template <typename T, std::size_t Capacity>
	requires(std::is_trivially_copyable_v<T> && Capacity > 0U)
class MessageQueue
{
      public:
	MessageQueue() noexcept
	{
		k_msgq_init(&queue_, reinterpret_cast<char *>(storage_.data()), sizeof(T),
			    Capacity);
	}

	MessageQueue(const MessageQueue &) = delete;
	MessageQueue &operator=(const MessageQueue &) = delete;

	template <typename Rep, typename Period>
	[[nodiscard]] std::expected<void, int> put(const T &value,
						   std::chrono::duration<Rep, Period> wait) noexcept
	{
		const int rc = k_msgq_put(&queue_, &value, detail::timeout(wait));
		return rc == 0 ? std::expected<void, int>{} : std::unexpected(rc);
	}

	[[nodiscard]] std::expected<void, int> try_put(const T &value) noexcept
	{
		const int rc = k_msgq_put(&queue_, &value, K_NO_WAIT);
		return rc == 0 ? std::expected<void, int>{} : std::unexpected(rc);
	}

	template <typename Rep, typename Period>
	[[nodiscard]] std::expected<T, int> get(std::chrono::duration<Rep, Period> wait) noexcept
	{
		T value{};
		const int rc = k_msgq_get(&queue_, &value, detail::timeout(wait));
		return rc == 0 ? std::expected<T, int>{value} : std::unexpected(rc);
	}

	[[nodiscard]] std::expected<T, int> try_get() noexcept
	{
		T value{};
		const int rc = k_msgq_get(&queue_, &value, K_NO_WAIT);
		return rc == 0 ? std::expected<T, int>{value} : std::unexpected(rc);
	}

	void purge() noexcept
	{
		k_msgq_purge(&queue_);
	}
	[[nodiscard]] std::size_t size() const noexcept
	{
		return k_msgq_num_used_get(&queue_);
	}
	[[nodiscard]] std::size_t available() const noexcept
	{
		return k_msgq_num_free_get(&queue_);
	}
	[[nodiscard]] k_msgq *native_handle() noexcept
	{
		return &queue_;
	}

      private:
	k_msgq queue_{};
	alignas(T) std::array<std::byte, sizeof(T) * Capacity> storage_{};
};

/** A statically allocated Zephyr system-workqueue item. */
class WorkItem
{
      public:
	using Handler = void (*)(void *) noexcept;

	explicit WorkItem(Handler handler, void *context = nullptr) noexcept
		: handler_{handler}, context_{context}
	{
		k_work_init(&work_, trampoline);
	}
	~WorkItem() noexcept
	{
		(void)k_work_cancel_sync(&work_, &sync_);
	}
	WorkItem(const WorkItem &) = delete;
	WorkItem &operator=(const WorkItem &) = delete;

	[[nodiscard]] std::expected<void, int> submit() noexcept
	{
		const int rc = k_work_submit(&work_);
		return rc >= 0 ? std::expected<void, int>{} : std::unexpected(rc);
	}
	[[nodiscard]] bool busy() const noexcept
	{
		return k_work_busy_get(&work_) != 0;
	}
	[[nodiscard]] k_work *native_handle() noexcept
	{
		return &work_;
	}

      private:
	static void trampoline(k_work *work) noexcept
	{
		auto *self = CONTAINER_OF(work, WorkItem, work_);
		if (self->handler_ != nullptr) {
			self->handler_(self->context_);
		}
	}
	k_work work_{};
	k_work_sync sync_{};
	Handler handler_{};
	void *context_{};
};

/** A callback-free periodic timer; consumers wait for expiration counts. */
class PeriodicTimer
{
      public:
	PeriodicTimer() noexcept
	{
		k_timer_init(&timer_, nullptr, nullptr);
	}
	~PeriodicTimer() noexcept
	{
		stop();
	}
	PeriodicTimer(const PeriodicTimer &) = delete;
	PeriodicTimer &operator=(const PeriodicTimer &) = delete;

	template <typename Rep1, typename Period1, typename Rep2, typename Period2>
	void start(std::chrono::duration<Rep1, Period1> initial,
		   std::chrono::duration<Rep2, Period2> period) noexcept
	{
		k_timer_start(&timer_, detail::timeout(initial), detail::timeout(period));
	}
	void stop() noexcept
	{
		k_timer_stop(&timer_);
	}
	[[nodiscard]] std::uint32_t wait() noexcept
	{
		return k_timer_status_sync(&timer_);
	}
	[[nodiscard]] std::uint32_t expirations() noexcept
	{
		return k_timer_status_get(&timer_);
	}
	[[nodiscard]] k_timer *native_handle() noexcept
	{
		return &timer_;
	}

      private:
	k_timer timer_{};
};

/** A statically allocated Zephyr thread with explicit start and lifetime. */
template <std::size_t StackSize> class StaticThread
{
      public:
	using Entry = void (*)(void *) noexcept;

	StaticThread() noexcept = default;
	StaticThread(const StaticThread &) = delete;
	StaticThread &operator=(const StaticThread &) = delete;
	~StaticThread() noexcept
	{
		if (started_ && k_thread_join(&thread_, K_NO_WAIT) != 0) {
			k_thread_abort(&thread_);
		}
	}

	[[nodiscard]] std::expected<void, int> start(Entry entry, void *context, int priority,
						     std::uint32_t options = 0U) noexcept
	{
		if (started_ || entry == nullptr) {
			return std::unexpected(-EINVAL);
		}
		entry_ = entry;
		context_ = context;
		const auto id = k_thread_create(&thread_, stack_, K_KERNEL_STACK_SIZEOF(stack_),
						entry_trampoline, this, nullptr, nullptr, priority,
						options, K_NO_WAIT);
		if (id == nullptr) {
			return std::unexpected(-ENOMEM);
		}
		started_ = true;
		return {};
	}

	[[nodiscard]] std::expected<void, int> join(std::chrono::milliseconds timeout) noexcept
	{
		if (!started_) {
			return {};
		}
		const int rc = k_thread_join(&thread_, detail::timeout(timeout));
		if (rc == 0) {
			started_ = false;
			return {};
		}
		return std::unexpected(rc);
	}
	void abort() noexcept
	{
		if (started_) {
			k_thread_abort(&thread_);
			started_ = false;
		}
	}
	[[nodiscard]] k_tid_t native_handle() noexcept
	{
		return &thread_;
	}

      private:
	static void entry_trampoline(void *self, void *, void *) noexcept
	{
		auto &thread = *static_cast<StaticThread *>(self);
		thread.entry_(thread.context_);
	}
	k_thread thread_{};
	K_KERNEL_STACK_MEMBER(stack_, StackSize);
	Entry entry_{};
	void *context_{};
	bool started_{};
};

} /* namespace zest */
