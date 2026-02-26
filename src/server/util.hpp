#pragma once

#include <bits/types/struct_iovec.h>
#include <sys/mman.h>
#include <unistd.h>

#include <iostream>
#include <optional>
#include <span>
#include <utility>

namespace ulab {

template<typename T, typename CleanupFn>
class RaiiCleanup {
public:
	RaiiCleanup() = default;
	RaiiCleanup(T resource) : resource(resource), cleanup_fn(CleanupFn{}) {}

	RaiiCleanup(T resource, CleanupFn cleanup_fn)
		: resource(resource), cleanup_fn(cleanup_fn) {}

	~RaiiCleanup() {
		cleanup_fn(resource);
	}

	RaiiCleanup(RaiiCleanup const&) = delete;
	auto operator=(RaiiCleanup const&) -> RaiiCleanup& = delete;

	RaiiCleanup(RaiiCleanup&& o) {
		*this = std::move(o);
	}
	auto operator=(RaiiCleanup &&o) -> RaiiCleanup& {
		std::swap(resource, o.resource);
		std::swap(cleanup_fn, o.cleanup_fn);
		return *this;
	}

	operator T&(this auto&& self) { return self.resource; }
	auto operator->(this auto&& self) -> T* { return &self.resource; }

private:
	T resource{};
	CleanupFn cleanup_fn{};
};

namespace detail {
struct MunmapCleanup {
	static auto operator()(std::span<std::byte> s) {if (!s.empty()) munmap(s.data(), s.size());}
};
}
using RaiiMunmap = RaiiCleanup<std::span<std::byte>, detail::MunmapCleanup>;
namespace detail {
struct CloseCleanup {
	static auto operator()(int fd) {if (fd > 0) close(fd);}
};
}
using RaiiClose = RaiiCleanup<int, detail::CloseCleanup>;

template<typename T>
class IntrusiveList {
public:
	IntrusiveList() {
		sentinel.next = &sentinel;
		sentinel.prev = &sentinel;
	}

	template<typename... Args>
	T& emplace_back(Args&&... args) {
		auto* new_node = new IntrusiveListNode<T>{std::forward<Args>(args)...};
		new_node->next = &sentinel;
		new_node->prev = sentinel.prev;
		sentinel.prev->next = new_node;
		sentinel.prev = new_node;
		return new_node->value.value();
	}

	void erase(T& item) {
		auto& opt = reinterpret_cast<std::optional<T>&>(item);
		auto* intrusive_node = reinterpret_cast<IntrusiveListNode<T>*>(&opt);
		intrusive_node->prev->next = intrusive_node->next;
		intrusive_node->next->prev = intrusive_node->prev;
		delete intrusive_node;
	}

private:
	template<typename U>
	struct IntrusiveListNode {

		IntrusiveListNode() : value{T{}} {}
		template<typename... Args>
		IntrusiveListNode(Args&&... args) : value{std::forward<Args>(args)...} {}

		std::optional<U> value{std::nullopt};
		IntrusiveListNode* next{nullptr};
		IntrusiveListNode* prev{nullptr};
	};
	static_assert(offsetof(IntrusiveListNode<T>, value) == 0, "IntrusiveListNode<T>::value must be at offset 0");
	IntrusiveListNode<T> sentinel{};
};

} // namespace ulab
