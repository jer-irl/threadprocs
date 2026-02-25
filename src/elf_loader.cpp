#include "elf_loader.hpp"

#include <memory>

namespace ulab {

LoadedElf::~LoadedElf() {
	if (map) {
		munmap(map, map_len);
		map = nullptr;
	}
	if (fd) {
		close(fd);
		fd = 0;
	}
}

auto LoadedElf::load_from_path(std::string_view path) -> std::expected<LoadedElf, int> {
	LoadedElf out{};

	constexpr size_t PAGE_SZ = 4096;
	auto page_down = [](size_t v) -> size_t { return v & ~size_t(PAGE_SZ - 1); };
	auto page_up   = [](size_t v) -> size_t { return (v + PAGE_SZ - 1) & ~size_t(PAGE_SZ - 1); };

	out.fd = open(std::string{path}.c_str(), O_RDONLY | O_CLOEXEC);
	if (out.fd < 0) {
		out.fd = 0;
		return std::unexpected{-errno};
	}

	// Read ELF header
	Elf64_Ehdr ehdr;
	ssize_t n = pread(out.fd, &ehdr, sizeof(ehdr), 0);
	if (n != (ssize_t)sizeof(ehdr)) {
		return std::unexpected{n < 0 ? -errno : -ENOEXEC};
	}

	if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 ||
	    ehdr.e_ident[EI_CLASS] != ELFCLASS64 ||
	    ehdr.e_type != ET_DYN) {
		return std::unexpected{-ENOEXEC};
	}

	// Read program headers
	size_t phsize = (size_t)ehdr.e_phentsize * ehdr.e_phnum;
	auto phbuf = std::make_unique<char[]>(phsize);
	if (!phbuf) {
		return std::unexpected{-ENOMEM};
	}

	n = pread(out.fd, phbuf.get(), phsize, ehdr.e_phoff);
	if (n != (ssize_t)phsize) {
		return std::unexpected{n < 0 ? -errno : -ENOEXEC};
	}

	// Phase 1: scan for address bounds, PT_INTERP
	size_t addr_min = SIZE_MAX, addr_max = 0;
	out.interp.clear();

	for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
		auto* ph = reinterpret_cast<Elf64_Phdr*>(phbuf.get() + i * ehdr.e_phentsize);

		if (ph->p_type == PT_INTERP) {
			char ibuf[256]{};
			size_t ilen = ph->p_filesz < sizeof(ibuf) ? ph->p_filesz : sizeof(ibuf) - 1;
			ssize_t r = pread(out.fd, ibuf, ilen, ph->p_offset);
			if (r > 0) { ibuf[r < (ssize_t)sizeof(ibuf) ? r : (ssize_t)sizeof(ibuf) - 1] = '\0'; out.interp = ibuf; }
		}

		if (ph->p_type != PT_LOAD) continue;
		if (ph->p_vaddr < addr_min)
			addr_min = ph->p_vaddr;
		if (ph->p_vaddr + ph->p_memsz > addr_max)
			addr_max = ph->p_vaddr + ph->p_memsz;
	}

	if (addr_max == 0) { return std::unexpected{-ENOEXEC}; }

	addr_min = page_down(addr_min);
	addr_max = page_up(addr_max);
	size_t map_len = addr_max - addr_min;

	// Phase 2: reserve contiguous address range
	void* map = mmap(nullptr, map_len, PROT_NONE,
	                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (map == MAP_FAILED) { return std::unexpected{-errno}; }

	auto* base = reinterpret_cast<char*>(map) - addr_min;

	// Phase 3: map each PT_LOAD segment
	out.phdr = nullptr;
	out.phnum = ehdr.e_phnum;
	out.phentsize = ehdr.e_phentsize;

	for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
		auto* ph = reinterpret_cast<Elf64_Phdr*>(phbuf.get() + i * ehdr.e_phentsize);
		if (ph->p_type != PT_LOAD) continue;

		int prot = 0;
		if (ph->p_flags & PF_R) prot |= PROT_READ;
		if (ph->p_flags & PF_W) prot |= PROT_WRITE;
		if (ph->p_flags & PF_X) prot |= PROT_EXEC;

		size_t seg_start = page_down(ph->p_vaddr);
		size_t seg_end   = page_up(ph->p_vaddr + ph->p_memsz);
		size_t off_start = page_down(ph->p_offset);

		// Map file-backed pages
		if (ph->p_filesz > 0) {
			size_t file_map_end = page_up(ph->p_vaddr + ph->p_filesz);
			if (file_map_end > seg_end) file_map_end = seg_end;
			size_t file_map_len = file_map_end - seg_start;

			void* seg = mmap(base + seg_start, file_map_len, prot,
			                 MAP_PRIVATE | MAP_FIXED, out.fd, off_start);
			if (seg == MAP_FAILED) {
				munmap(map, map_len);
				return std::unexpected{-errno};
			}
		}

		// BSS: zero partial page, anonymous-map remaining pages
		if (ph->p_memsz > ph->p_filesz) {
			size_t brk   = reinterpret_cast<size_t>(base + ph->p_vaddr + ph->p_filesz);
			size_t pgbrk = page_up(brk);

			if (ph->p_filesz > 0 && (prot & PROT_WRITE) && pgbrk > brk)
				memset(reinterpret_cast<void*>(brk), 0, pgbrk - brk);

			size_t seg_end_addr = reinterpret_cast<size_t>(base + seg_end);
			if (pgbrk < seg_end_addr) {
				void* bss = mmap(reinterpret_cast<void*>(pgbrk),
				                 seg_end_addr - pgbrk, prot,
				                 MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
				if (bss == MAP_FAILED) {
					munmap(map, map_len);
					return std::unexpected{-errno};
				}
			}
		}

		// Locate in-memory program headers
		if (!out.phdr &&
		    ehdr.e_phoff >= ph->p_offset &&
		    ehdr.e_phoff + phsize <= ph->p_offset + ph->p_filesz) {
			out.phdr = base + ph->p_vaddr + (ehdr.e_phoff - ph->p_offset);
		}
	}

	out.base = base;
	out.entry = base + ehdr.e_entry;
	out.map = map;
	out.map_len = map_len;

	if (!out.phdr) {
		// Fallback: phdr might be at e_phoff from base
		out.phdr = base + ehdr.e_phoff;
	}

	return out;
}

} // namespace ulab
