#pragma once

#include "server/util.hpp"
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>

namespace ulab {

class LoadedElf {
	LoadedElf() = default;
public:
	static auto load_from_path(std::string_view path) -> std::expected<LoadedElf, int>;

	LoadedElf(const LoadedElf&) = delete;
	LoadedElf& operator=(const LoadedElf&) = delete;
	LoadedElf(LoadedElf&& other) {
		*this = std::move(other);
	}
	LoadedElf& operator=(LoadedElf&& other) {
		std::swap(base, other.base);
		std::swap(entry, other.entry);
		std::swap(phdr, other.phdr);
		std::swap(phnum, other.phnum);
		std::swap(phentsize, other.phentsize);
		std::swap(map, other.map);
		std::swap(interp, other.interp);
		std::swap(fd, other.fd);
		return *this;
	}

	void* base{};        // base offset: real_addr = (char*)base + vaddr
	void* entry{};       // computed entry point
	void* phdr{};        // program headers in memory
	uint16_t phnum{};
	uint16_t phentsize{};
	RaiiMunmap map{std::span<std::byte>{}};
	std::string interp; // PT_INTERP path (empty if none)
	
	RaiiClose fd{-1};
};

} // namespace ulab
