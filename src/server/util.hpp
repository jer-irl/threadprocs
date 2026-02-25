#pragma once

#include <utility>

namespace ulab {

template<typename T, typename CleanupFn>
class raii_cleanup {
public:
	raii_cleanup(T resource, CleanupFn cleanup_fn)
		: resource(resource), cleanup_fn(cleanup_fn) {}

	~raii_cleanup() {
		cleanup_fn(resource);
	}

	
	operator T&(this auto&& self) { return self.resource; }

private:
	T resource;
	CleanupFn cleanup_fn;
};

} // namespace ulab
