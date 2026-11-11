#include "alloc.hpp"
#include <cstdint>

#ifdef Y8_USE_EXTMEM
tlsf_t global_heap = nullptr;
tlsf_t extmem_heap = nullptr;

static inline bool is_psram_ptr(const void *ptr) {
	const auto addr = reinterpret_cast<std::uintptr_t>(ptr);
	return (addr >= 0x11000000u && addr < 0x12000000u);
}
#endif

extern "C" {
#ifdef Y8_USE_EXTMEM

// Allocate from fast SRAM first; fall back to PSRAM only if SRAM is exhausted
[[gnu::used]] void *__wrap_malloc(size_t size) {
	if (global_heap) {
		void *p = tlsf_malloc(global_heap, size);
		if (p) return p;
	}
	if (extmem_heap) {
		return tlsf_malloc(extmem_heap, size);
	}
	return nullptr;
}

// Free to corresponding heap based on memory address
[[gnu::used]] void __wrap_free(void *ptr) {
	if (!ptr) return;
	if (is_psram_ptr(ptr)) {
		if (extmem_heap) tlsf_free(extmem_heap, ptr);
	} else {
		if (global_heap) tlsf_free(global_heap, ptr);
	}
}

// Reallocate preserving fast SRAM placement when possible
[[gnu::used]] void *__wrap_realloc(void *ptr, size_t new_size) {
	if (!ptr) {
		return __wrap_malloc(new_size);
	}
	if (new_size == 0) {
		__wrap_free(ptr);
		return nullptr;
	}

	if (is_psram_ptr(ptr)) {
		if (!extmem_heap) return nullptr;
		return tlsf_realloc(extmem_heap, ptr, new_size);
	}

	if (!global_heap) return nullptr;
	void *p = tlsf_realloc(global_heap, ptr, new_size);
	if (p) return p;

	// SRAM is full or cannot expand: migrate to PSRAM
	if (extmem_heap) {
		void *new_p = tlsf_malloc(extmem_heap, new_size);
		if (new_p) {
			size_t old_size = tlsf_block_size(ptr);
			size_t copy_size = (old_size < new_size) ? old_size : new_size;
			std::memcpy(new_p, ptr, copy_size);
			tlsf_free(global_heap, ptr);
			return new_p;
		}
	}

	return nullptr;
}

#endif
}

void *y8_lua_realloc(void *ud, void *ptr, size_t osize, size_t nsize,
                     [[maybe_unused]] bool must_not_fail) {
	(void)ud;

	if (nsize == 0) {
#ifdef Y8_USE_EXTMEM
		__wrap_free(ptr);
#else
		free(ptr);
#endif
		return nullptr;
	} else if (ptr == nullptr) {
#ifdef Y8_USE_EXTMEM
		return __wrap_malloc(nsize);
#else
		return malloc(nsize);
#endif
	} else if (nsize == osize) {
		return ptr;
	} else // (nsize < osize) || (nsize > osize)
	{
#ifdef Y8_USE_EXTMEM
		return __wrap_realloc(ptr, nsize);
#else
		return realloc(ptr, nsize);
#endif
	}
}

// void *operator new(std::size_t sz);
// void operator delete([[maybe_unused]] void *ptr) {}
// void operator delete([[maybe_unused]] void *ptr, std::size_t) noexcept {}
