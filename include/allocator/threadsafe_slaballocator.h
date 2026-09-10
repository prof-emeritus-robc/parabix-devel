#ifndef THREADSAFE_SLABALLOCATOR_H
#define THREADSAFE_SLABALLOCATOR_H

#include <atomic>
#include <boost/intrusive/detail/math.hpp>
#include <limits>
#include <new>

#ifdef _MSC_VER
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

// This class must be created at the start of a thread that can create objects inheriting the
// ThreadSafeSlabAllocated or using the StdSlabAllocatorProxy
class EXPORT ThreadSafeSlabAllocator {

    using size_type = unsigned long;

    constexpr static size_type SLAB_SIZE = 1024UL * 1024UL;

    struct SlabNode {
        SlabNode * Next;
    };

#ifdef _MSC_VER
    __declspec(selectany) static inline __declspec(thread)
#else
    __attribute__((visibility("default")))
    __attribute__((tls_model("initial-exec")))
    static inline __thread
#endif
    ThreadSafeSlabAllocator * CurrentThreadAllocatorPtr = nullptr;

    static inline std::atomic<SlabNode *> PendingDeletions{nullptr};

#ifdef _MSC_VER
    #pragma section(".CRT$XCU", read)
    __declspec(allocate(".CRT$XCU"))
#else
    __attribute__((visibility("default")))
#endif
    static ThreadSafeSlabAllocator __STATIC_PROCESS_THREAD_ALLOCATOR;

public:

    template<typename T>
    static inline T * allocate_array_of(const size_t size) noexcept {
        return reinterpret_cast<T*>(allocate(size * sizeof(T), alignof(T)));
    }

    // NOTE: this is intentionally unsafe to non power of 2 alignments
    static inline void* allocate(const size_type size, const size_type align) noexcept {
        assert (size > 0);
        assert (boost::intrusive::detail::is_pow2(align) && align > 0);
        const auto al = align < alignof(std::max_align_t) ? alignof(std::max_align_t) : align;
        ThreadSafeSlabAllocator * const a = CurrentThreadAllocatorPtr;
        assert ("no ThreadSafeSlabAllocator constructed in scope?" && a);
        const uintptr_t p = reinterpret_cast<uintptr_t>(a->BumpPtr);
        assert (p || a->EndPtr == nullptr);
        uintptr_t addr = (p + al - 1UL) & ~(al - 1UL);
        assert (addr >= p || a->EndPtr == nullptr);
        const size_type padding = addr - p;
        size_type s = size + padding;
        if (BOOST_UNLIKELY(a->BumpPtr + s > a->EndPtr)) {
            a->AllocateNewSlab(s);
            const uintptr_t p = reinterpret_cast<uintptr_t>(a->BumpPtr);
            assert (p);
            addr = (p + al - 1UL) & ~(al - 1UL);
            assert (addr >= p);
            const size_t padding = addr - p;
            s = size + padding;
        }
        assert (addr);
        assert (a->BumpPtr && a->EndPtr);
        a->BumpPtr += s;
        assert (a->BumpPtr <= a->EndPtr);
        return reinterpret_cast<void*>(addr);
    }

    ThreadSafeSlabAllocator() noexcept {
        assert ("ThreadSafeSlabAllocator was already constructed?" && CurrentThreadAllocatorPtr == nullptr);
        CurrentThreadAllocatorPtr = this;
    }

    ~ThreadSafeSlabAllocator() noexcept {
        // Rather than immediately freeing all of the slabs, we enqueue them for deletion later
        // when the slab allocator is deconstructed. This ensures we can control when all of the
        // allocated objects are freed regardless of scope or threading.
        CurrentThreadAllocatorPtr = nullptr;
        SlabNode * const current = CurrentSlab;
        if (current) {
            SlabNode * tail = current;
            while (tail->Next) {
                tail = tail->Next;
            }
            SlabNode * global = PendingDeletions.load(std::memory_order_relaxed);
            do {
                tail->Next = global;
            } while (!PendingDeletions.compare_exchange_weak(global, current, std::memory_order_release, std::memory_order_relaxed));
        }
    }

    static void PurgeAll() noexcept {
        auto d = PendingDeletions.exchange(nullptr, std::memory_order_acquire);
        while (d) {
            auto n = d->Next;
            std::free(reinterpret_cast<void*>(d));
            d = n;
        }
    }

private:

    void AllocateNewSlab(const size_t required) noexcept;

private:
    uint8_t * BumpPtr = nullptr;
    uint8_t * EndPtr = nullptr;
    SlabNode * CurrentSlab = nullptr;
};

class SlabAllocatedObject {
public:

    using size_type = unsigned long;

    void * operator new(size_type size, void *ptr) noexcept {
        assert (ptr); return ptr;
    }
    void * operator new[](size_type size, void *ptr) noexcept {
        assert (ptr); return ptr;
    }

    void * operator new(size_type size) {
        return ThreadSafeSlabAllocator::allocate(size, alignof(std::max_align_t));
    }
    void * operator new[](size_type size) {
        return ThreadSafeSlabAllocator::allocate(size, alignof(std::max_align_t));
    }
    template<typename... Args>
    void * operator new(size_type size, Args&&...) {
        return ThreadSafeSlabAllocator::allocate(size, alignof(std::max_align_t));
    }

    void * operator new(size_type size, std::align_val_t a) {
        return ThreadSafeSlabAllocator::allocate(size, static_cast<size_type>(a));
    }
    void * operator new[](size_type size, std::align_val_t a) {
        return ThreadSafeSlabAllocator::allocate(size, static_cast<size_type>(a));
    }
    template<typename... Args>
    void * operator new(size_type size, std::align_val_t a, Args&&...) {
        return ThreadSafeSlabAllocator::allocate(size, static_cast<size_type>(a));
    }

    void operator delete(void *) noexcept {}
    void operator delete[](void *) noexcept {}
    template<typename... Args> void operator delete(void *, Args&&...) noexcept {}

    void operator delete(void *, std::align_val_t) noexcept {}
    void operator delete[](void *, std::align_val_t) noexcept {}
    template<typename... Args> void operator delete(void *, std::align_val_t, Args&&...) noexcept {}

    void operator delete(void *, void *) noexcept {}
    void operator delete[](void *, void *) noexcept {}

    SlabAllocatedObject() noexcept = default;

    virtual ~SlabAllocatedObject() noexcept;
};

#undef EXPORT

#define USE_SLAB_ALLOCATED_OBJECT_MEMORY_OPERATORS \
    using SlabAllocatedObject::operator new; \
    using SlabAllocatedObject::operator new[]; \
    using SlabAllocatedObject::operator delete; \
    using SlabAllocatedObject::operator delete[]; \
    [[no_unique_address]] SlabAllocatedObject __ebo_breaker;

template <typename T = uint8_t>
class StdSlabAllocatorProxy {
public:

    using value_type = T;
    using pointer = value_type*;
    using const_pointer = const value_type*;
    using reference = value_type&;
    using const_reference = const value_type&;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    template<class U>
    struct rebind {
        typedef StdSlabAllocatorProxy<U> other;
    };

    template<typename Type = T>
    inline Type * allocate(const size_type n, const_pointer = nullptr) noexcept {
        static_assert(sizeof(Type) > 0, "Cannot allocate a zero-length type.");
        if (BOOST_UNLIKELY(n == 0)) return nullptr;
        return static_cast<Type *>(ThreadSafeSlabAllocator::allocate(n * sizeof(Type), alignof(std::max_align_t)));
    }

    template<typename Type = T>
    inline Type * Allocate(const size_type n, const_pointer = nullptr) noexcept {
        static_assert(sizeof(Type) > 0, "Cannot allocate a zero-length type.");
        if (BOOST_UNLIKELY(n == 0)) return nullptr;
        return static_cast<Type *>(ThreadSafeSlabAllocator::allocate(n * sizeof(Type), alignof(std::max_align_t)));
    }

    template<typename Type = T>
    inline Type * aligned_allocate(const size_type n, const size_t align, const_pointer = nullptr) noexcept {
        static_assert(sizeof(Type) > 0, "Cannot allocate a zero-length type.");
        if (BOOST_UNLIKELY(n == 0)) return nullptr;
        return static_cast<Type *>(ThreadSafeSlabAllocator::allocate(n * sizeof(Type), align));
    }

    template<typename Type = T>
    inline void deallocate(Type * /*p */, size_type /* size */ = 0) noexcept {

    }

    inline size_type max_size() const {
        return std::numeric_limits<size_type>::max();
    }

    template<typename Type = T>
    inline bool operator==(StdSlabAllocatorProxy<Type> const & other) const noexcept {
        return this == &other;
    }

    template<typename Type = T>
    inline bool operator!=(StdSlabAllocatorProxy<Type> const & other) const noexcept {
        return this != &other;
    }

    inline StdSlabAllocatorProxy() noexcept {}
    inline StdSlabAllocatorProxy(const StdSlabAllocatorProxy &) noexcept = default;
    inline StdSlabAllocatorProxy(StdSlabAllocatorProxy &&) noexcept = default;
    template <class U> inline StdSlabAllocatorProxy (const StdSlabAllocatorProxy<U> &) noexcept { }
};


#endif // THREADSAFE_SLABALLOCATOR_H
