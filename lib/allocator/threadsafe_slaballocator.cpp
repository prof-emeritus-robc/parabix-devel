#include "allocator/threadsafe_slaballocator.h"
#include <boost/interprocess/mapped_region.hpp>

ThreadSafeSlabAllocator ThreadSafeSlabAllocator::__STATIC_PROCESS_THREAD_ALLOCATOR{};

void ThreadSafeSlabAllocator::AllocateNewSlab(const size_t required) noexcept {
    constexpr auto ALIGN_MASK = alignof(std::max_align_t) - 1;
    constexpr auto HEADER_SIZE = (sizeof(SlabNode) + ALIGN_MASK) & ~ALIGN_MASK;
    const auto PAGE_SIZE = boost::interprocess::mapped_region::get_page_size();
    assert ((SLAB_SIZE % PAGE_SIZE) == 0);
    const auto size = ((required + HEADER_SIZE) + (SLAB_SIZE - 1)) & ~(SLAB_SIZE - 1);
    uint8_t * const ptr = static_cast<uint8_t*>(std::aligned_alloc(PAGE_SIZE, size));
    SlabNode * const newSlab = reinterpret_cast<SlabNode*>(ptr);
    newSlab->Next = CurrentSlab;
    CurrentSlab = newSlab;
    BumpPtr = ptr + HEADER_SIZE;
    EndPtr = ptr + size;
}

SlabAllocatedObject::~SlabAllocatedObject() noexcept = default;
