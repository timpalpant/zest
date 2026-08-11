/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>
#include <zest/function.hpp>

#include <zephyr/kernel.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace zest
{

namespace detail
{

/**
 * Convert a chrono duration to a Zephyr timeout.
 *
 * Sub-millisecond waits are preserved by converting through microseconds, and
 * anything finer is rounded *up*, so a requested wait is never shorter than
 * asked.
 *
 * `duration::max()` means wait forever; a non-positive duration means do not wait.
 */
template <typename Rep, typename Period>
[[nodiscard]] constexpr k_timeout_t timeout(std::chrono::duration<Rep, Period> value) noexcept
{
	using Duration = std::chrono::duration<Rep, Period>;

	if (value >= Duration::max()) {
		return K_FOREVER;
	}
	if (value <= Duration::zero()) {
		return K_NO_WAIT;
	}

	/* Round up to the next whole microsecond. */
	const std::int64_t microseconds =
		std::chrono::ceil<std::chrono::microseconds>(value).count();
	if (microseconds <= 0) {
		return K_NO_WAIT;
	}
	/* Guard against overflowing Zephyr's tick conversion. */
	constexpr std::int64_t kMaxMicroseconds = std::int64_t{1} << 52;
	if (microseconds >= kMaxMicroseconds) {
		return K_FOREVER;
	}
	return K_USEC(microseconds);
}

} /* namespace detail */

/** Monotonic time since boot, as the kernel reports it. */
[[nodiscard]] inline std::chrono::milliseconds uptime() noexcept
{
	return std::chrono::milliseconds{k_uptime_get()};
}

/**
 * Sleep the calling thread.
 *
 * Sub-millisecond requests go through `k_usleep`, so `sleep_for(250us)` actually
 * waits instead of returning immediately.
 */
template <typename Rep, typename Period>
void sleep_for(std::chrono::duration<Rep, Period> duration) noexcept
{
	if (duration <= std::chrono::duration<Rep, Period>::zero()) {
		return;
	}
	const std::int64_t microseconds =
		std::chrono::ceil<std::chrono::microseconds>(duration).count();
	if (microseconds < 1000) {
		(void)k_usleep(static_cast<std::int32_t>(microseconds));
		return;
	}
	(void)k_sleep(detail::timeout(duration));
}

/** A Zephyr mutex. Prefer `ScopedLock` over calling `lock()` directly. */
class Mutex
{
      public:
	Mutex() noexcept
	{
		k_mutex_init(&mutex_);
	}
	Mutex(const Mutex &) = delete;
	Mutex &operator=(const Mutex &) = delete;

	/** Acquire the mutex, waiting as long as necessary. */
	void lock() noexcept
	{
		(void)k_mutex_lock(&mutex_, K_FOREVER);
	}

	/** Acquire the mutex, giving up after @p wait. */
	template <typename Rep, typename Period>
	[[nodiscard]] Result<> lock_for(std::chrono::duration<Rep, Period> wait) noexcept
	{
		return check(k_mutex_lock(&mutex_, detail::timeout(wait)));
	}

	/** Acquire the mutex only if it is free. */
	[[nodiscard]] bool try_lock() noexcept
	{
		return k_mutex_lock(&mutex_, K_NO_WAIT) == 0;
	}

	void unlock() noexcept
	{
		(void)k_mutex_unlock(&mutex_);
	}

	[[nodiscard]] k_mutex *native_handle() noexcept
	{
		return &mutex_;
	}

      private:
	k_mutex mutex_{};
};

/**
 * Hold a lock for the enclosing scope.
 *
 * Releases on every exit path, including an early return that reports a failure
 * --- which is exactly where a hand-written unlock gets forgotten.
 */
class ScopedLock
{
      public:
	explicit ScopedLock(Mutex &mutex) noexcept : mutex_{&mutex}
	{
		mutex_->lock();
	}
	~ScopedLock() noexcept
	{
		unlock();
	}
	ScopedLock(const ScopedLock &) = delete;
	ScopedLock &operator=(const ScopedLock &) = delete;
	ScopedLock(ScopedLock &&other) noexcept : mutex_{other.mutex_}
	{
		other.mutex_ = nullptr;
	}
	ScopedLock &operator=(ScopedLock &&) = delete;

	/** Release early, before the scope ends. Idempotent. */
	void unlock() noexcept
	{
		if (mutex_ != nullptr) {
			mutex_->unlock();
			mutex_ = nullptr;
		}
	}

      private:
	Mutex *mutex_;
};

/** A Zephyr counting semaphore. */
class Semaphore
{
      public:
	explicit Semaphore(unsigned int initial = 0U, unsigned int limit = 1U) noexcept
	{
		k_sem_init(&semaphore_, initial, limit);
	}
	Semaphore(const Semaphore &) = delete;
	Semaphore &operator=(const Semaphore &) = delete;

	/** Wait for the semaphore. A `duration::max()` wait blocks forever. */
	template <typename Rep, typename Period>
	[[nodiscard]] Result<> take(std::chrono::duration<Rep, Period> wait) noexcept
	{
		return check(k_sem_take(&semaphore_, detail::timeout(wait)));
	}

	[[nodiscard]] bool try_take() noexcept
	{
		return k_sem_take(&semaphore_, K_NO_WAIT) == 0;
	}

	/** Signal the semaphore. Safe to call from an ISR. */
	void give() noexcept
	{
		k_sem_give(&semaphore_);
	}

	void reset() noexcept
	{
		k_sem_reset(&semaphore_);
	}

	[[nodiscard]] unsigned int count() const noexcept
	{
		return k_sem_count_get(const_cast<k_sem *>(&semaphore_));
	}

	[[nodiscard]] k_sem *native_handle() noexcept
	{
		return &semaphore_;
	}

      private:
	k_sem semaphore_{};
};

/** A fixed-capacity, allocation-free Zephyr message queue. */
template <typename T, std::size_t Capacity>
	requires(std::is_trivially_copyable_v<T> && Capacity > 0U)
class MessageQueue
{
      public:
	using value_type = T;

	MessageQueue() noexcept
	{
		k_msgq_init(&queue_, reinterpret_cast<char *>(storage_.data()), sizeof(T),
			    Capacity);
	}

	MessageQueue(const MessageQueue &) = delete;
	MessageQueue &operator=(const MessageQueue &) = delete;

	/** Append a message, waiting up to @p wait for room. */
	template <typename Rep, typename Period>
	[[nodiscard]] Result<> put(const T &value, std::chrono::duration<Rep, Period> wait) noexcept
	{
		return check(k_msgq_put(&queue_, &value, detail::timeout(wait)));
	}

	/** Append a message without waiting. Safe to call from an ISR. */
	[[nodiscard]] Result<> try_put(const T &value) noexcept
	{
		return check(k_msgq_put(&queue_, &value, K_NO_WAIT));
	}

	/** Remove the oldest message, waiting up to @p wait for one. */
	template <typename Rep, typename Period>
	[[nodiscard]] Result<T> get(std::chrono::duration<Rep, Period> wait) noexcept
	{
		T value{};
		if (const int rc = k_msgq_get(&queue_, &value, detail::timeout(wait)); rc != 0) {
			return fail(rc);
		}
		return value;
	}

	/** Remove the oldest message without waiting. */
	[[nodiscard]] Result<T> try_get() noexcept
	{
		T value{};
		if (const int rc = k_msgq_get(&queue_, &value, K_NO_WAIT); rc != 0) {
			return fail(rc);
		}
		return value;
	}

	/** Read the oldest message without removing it. */
	[[nodiscard]] Result<T> peek() const noexcept
	{
		T value{};
		if (const int rc = k_msgq_peek(const_cast<k_msgq *>(&queue_), &value); rc != 0) {
			return fail(rc);
		}
		return value;
	}

	void purge() noexcept
	{
		k_msgq_purge(&queue_);
	}
	[[nodiscard]] std::size_t size() const noexcept
	{
		return k_msgq_num_used_get(const_cast<k_msgq *>(&queue_));
	}
	[[nodiscard]] std::size_t available() const noexcept
	{
		return k_msgq_num_free_get(const_cast<k_msgq *>(&queue_));
	}
	[[nodiscard]] static constexpr std::size_t capacity() noexcept
	{
		return Capacity;
	}
	[[nodiscard]] k_msgq *native_handle() noexcept
	{
		return &queue_;
	}

      private:
	k_msgq queue_{};
	alignas(T) std::array<std::byte, sizeof(T) * Capacity> storage_{};
};

/**
 * The callback a work item or thread runs.
 *
 * An `InplaceFunction` rather than a bare function pointer plus a `void *`, so a
 * lambda capturing `this` can be submitted directly. Nothing is allocated: the
 * closure lives inside the work item.
 */
using WorkHandler = InplaceFunction<void() noexcept, 3 * sizeof(void *)>;

/** A statically allocated work item on Zephyr's system workqueue. */
class WorkItem
{
      public:
	WorkItem() noexcept
	{
		k_work_init(&work_, trampoline);
	}

	template <typename F>
		requires(!std::is_same_v<std::decay_t<F>, WorkItem>)
	explicit WorkItem(F &&handler) noexcept : handler_{std::forward<F>(handler)}
	{
		k_work_init(&work_, trampoline);
	}

	~WorkItem() noexcept
	{
		(void)k_work_cancel_sync(&work_, &sync_);
	}
	WorkItem(const WorkItem &) = delete;
	WorkItem &operator=(const WorkItem &) = delete;

	/** Replace the handler. Must not race with a pending run. */
	template <typename F> void set_handler(F &&handler) noexcept
	{
		handler_ = std::forward<F>(handler);
	}

	/** Queue on the system workqueue. Safe to call from an ISR. */
	[[nodiscard]] Result<> submit() noexcept
	{
		return check(k_work_submit(&work_));
	}

	/** Queue on a specific workqueue. */
	[[nodiscard]] Result<> submit_to(k_work_q &queue) noexcept
	{
		return check(k_work_submit_to_queue(&queue, &work_));
	}

	/** Cancel a pending item and wait for any in-progress run to finish. */
	bool cancel() noexcept
	{
		return k_work_cancel_sync(&work_, &sync_);
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
		if (self->handler_) {
			self->handler_();
		}
	}

	k_work work_{};
	k_work_sync sync_{};
	WorkHandler handler_{};
};

/**
 * A work item that can be scheduled to run later.
 *
 * This is the shape most retry, timeout and debounce logic needs.
 */
class DelayableWorkItem
{
      public:
	DelayableWorkItem() noexcept
	{
		k_work_init_delayable(&work_, trampoline);
	}

	template <typename F>
		requires(!std::is_same_v<std::decay_t<F>, DelayableWorkItem>)
	explicit DelayableWorkItem(F &&handler) noexcept : handler_{std::forward<F>(handler)}
	{
		k_work_init_delayable(&work_, trampoline);
	}

	~DelayableWorkItem() noexcept
	{
		(void)k_work_cancel_delayable_sync(&work_, &sync_);
	}
	DelayableWorkItem(const DelayableWorkItem &) = delete;
	DelayableWorkItem &operator=(const DelayableWorkItem &) = delete;

	template <typename F> void set_handler(F &&handler) noexcept
	{
		handler_ = std::forward<F>(handler);
	}

	/** Run after @p delay, keeping any deadline already pending. */
	template <typename Rep, typename Period>
	[[nodiscard]] Result<> schedule(std::chrono::duration<Rep, Period> delay) noexcept
	{
		return check(k_work_schedule(&work_, detail::timeout(delay)));
	}

	/** Run after @p delay, replacing any deadline already pending. */
	template <typename Rep, typename Period>
	[[nodiscard]] Result<> reschedule(std::chrono::duration<Rep, Period> delay) noexcept
	{
		return check(k_work_reschedule(&work_, detail::timeout(delay)));
	}

	template <typename Rep, typename Period>
	[[nodiscard]] Result<> schedule_on(k_work_q &queue,
					   std::chrono::duration<Rep, Period> delay) noexcept
	{
		return check(k_work_schedule_for_queue(&queue, &work_, detail::timeout(delay)));
	}

	/** Cancel and wait for any in-progress run to finish. */
	bool cancel() noexcept
	{
		return k_work_cancel_delayable_sync(&work_, &sync_);
	}

	[[nodiscard]] bool pending() const noexcept
	{
		return k_work_delayable_busy_get(&work_) != 0;
	}

	/** Time left before the item runs. */
	[[nodiscard]] std::chrono::milliseconds remaining() const noexcept
	{
		return std::chrono::milliseconds{
			k_ticks_to_ms_floor64(k_work_delayable_remaining_get(&work_))};
	}

	[[nodiscard]] k_work_delayable *native_handle() noexcept
	{
		return &work_;
	}

      private:
	static void trampoline(k_work *work) noexcept
	{
		auto *delayable = k_work_delayable_from_work(work);
		auto *self = CONTAINER_OF(delayable, DelayableWorkItem, work_);
		if (self->handler_) {
			self->handler_();
		}
	}

	k_work_delayable work_{};
	k_work_sync sync_{};
	WorkHandler handler_{};
};

/**
 * A dedicated workqueue with its own stack and priority.
 *
 * Long or blocking work does not belong on the system workqueue: stalling it
 * delays every other subsystem that shares it, networking included.
 */
template <std::size_t StackSize> class WorkQueue
{
      public:
	WorkQueue() noexcept = default;
	WorkQueue(const WorkQueue &) = delete;
	WorkQueue &operator=(const WorkQueue &) = delete;

	/**
	 * Start the queue's thread.
	 *
	 * @p name is applied to the thread when Zephyr's thread names are enabled,
	 * which is what makes a fault dump or the thread analyzer readable.
	 */
	[[nodiscard]] Result<> start(int priority, const char *name = "zest_wq") noexcept
	{
		if (started_) {
			return fail(errors::already);
		}
		const k_work_queue_config config{
			.name = name,
			.no_yield = false,
			.essential = false,
		};
		k_work_queue_start(&queue_, stack_, K_KERNEL_STACK_SIZEOF(stack_), priority,
				   &config);
		started_ = true;
		return {};
	}

	/** Submit an item to this queue. */
	[[nodiscard]] Result<> submit(WorkItem &item) noexcept
	{
		if (!started_) {
			return fail(errors::not_connected);
		}
		return item.submit_to(queue_);
	}

	template <typename Rep, typename Period>
	[[nodiscard]] Result<> schedule(DelayableWorkItem &item,
					std::chrono::duration<Rep, Period> delay) noexcept
	{
		if (!started_) {
			return fail(errors::not_connected);
		}
		return item.schedule_on(queue_, delay);
	}

	/** Wait for everything currently queued to finish. */
	[[nodiscard]] Result<> drain(bool block_submissions = false) noexcept
	{
		if (!started_) {
			return fail(errors::not_connected);
		}
		return check(k_work_queue_drain(&queue_, block_submissions));
	}

	[[nodiscard]] bool started() const noexcept
	{
		return started_;
	}
	[[nodiscard]] k_work_q *native_handle() noexcept
	{
		return &queue_;
	}

      private:
	k_work_q queue_{};
	K_KERNEL_STACK_MEMBER(stack_, StackSize);
	bool started_{false};
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

	/** Start a timer whose first expiry is one full period away. */
	template <typename Rep, typename Period>
	void start(std::chrono::duration<Rep, Period> period) noexcept
	{
		start(period, period);
	}

	void stop() noexcept
	{
		k_timer_stop(&timer_);
	}

	/** Block until the timer expires, returning the expiration count. */
	[[nodiscard]] std::uint32_t wait() noexcept
	{
		return k_timer_status_sync(&timer_);
	}

	/** Read and clear the expiration count without blocking. */
	[[nodiscard]] std::uint32_t expirations() noexcept
	{
		return k_timer_status_get(&timer_);
	}

	/** Time until the next expiry. */
	[[nodiscard]] std::chrono::milliseconds remaining() const noexcept
	{
		return std::chrono::milliseconds{
			k_ticks_to_ms_floor64(k_timer_remaining_ticks(&timer_))};
	}

	[[nodiscard]] k_timer *native_handle() noexcept
	{
		return &timer_;
	}

      private:
	k_timer timer_{};
};

/**
 * A statically allocated Zephyr thread with explicit start and lifetime.
 *
 * The stack is a member, and Zephyr's stack macros carry architecture-specific
 * alignment that only holds for objects with static storage duration. Declare
 * instances at file or class scope, never on another thread's stack. A
 * `static_assert` cannot detect this, so it is a contract rather than a check.
 */
template <std::size_t StackSize> class StaticThread
{
      public:
	using Entry = InplaceFunction<void() noexcept, 3 * sizeof(void *)>;

	StaticThread() noexcept = default;
	StaticThread(const StaticThread &) = delete;
	StaticThread &operator=(const StaticThread &) = delete;
	~StaticThread() noexcept
	{
		if (started_ && k_thread_join(&thread_, K_NO_WAIT) != 0) {
			k_thread_abort(&thread_);
		}
	}

	/**
	 * Create and start the thread.
	 *
	 * @p name is applied with `k_thread_name_set` when Zephyr's thread names
	 * are enabled; an unnamed thread is unidentifiable in a fault dump.
	 */
	template <typename F>
	[[nodiscard]] Result<> start(F &&entry, int priority, const char *name = nullptr,
				     std::uint32_t options = 0U) noexcept
	{
		if (started_) {
			return fail(errors::already);
		}
		entry_ = std::forward<F>(entry);
		if (!entry_) {
			return fail(errors::invalid_argument);
		}

		const auto id = k_thread_create(&thread_, stack_, K_KERNEL_STACK_SIZEOF(stack_),
						entry_trampoline, this, nullptr, nullptr, priority,
						options, K_NO_WAIT);
		if (id == nullptr) {
			return fail(errors::no_memory);
		}
		if (name != nullptr) {
			(void)k_thread_name_set(id, name);
		}
		started_ = true;
		return {};
	}

	template <typename Rep, typename Period>
	[[nodiscard]] Result<> join(std::chrono::duration<Rep, Period> wait) noexcept
	{
		if (!started_) {
			return {};
		}
		if (const int rc = k_thread_join(&thread_, detail::timeout(wait)); rc != 0) {
			return fail(rc);
		}
		started_ = false;
		return {};
	}

	void abort() noexcept
	{
		if (started_) {
			k_thread_abort(&thread_);
			started_ = false;
		}
	}

	[[nodiscard]] bool started() const noexcept
	{
		return started_;
	}
	[[nodiscard]] k_tid_t native_handle() noexcept
	{
		return &thread_;
	}
	[[nodiscard]] static constexpr std::size_t stack_size() noexcept
	{
		return StackSize;
	}

      private:
	static void entry_trampoline(void *self, void *, void *) noexcept
	{
		auto &thread = *static_cast<StaticThread *>(self);
		if (thread.entry_) {
			thread.entry_();
		}
	}

	k_thread thread_{};
	K_KERNEL_STACK_MEMBER(stack_, StackSize);
	Entry entry_{};
	bool started_{};
};

} /* namespace zest */
