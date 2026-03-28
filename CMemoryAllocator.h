#pragma once
#define CMEMORYPOOL_ISDEBUG 0

#define NAMESPACE_MEMORYPOOL_BEGIN  namespace kuodafu {
#define NAMESPACE_MEMORYPOOL_END    }

// 平台检测
#if defined(_WIN32) || defined(_WIN64)
    #define _MEMPOOL_PLATFORM_WINDOWS 1
    #include <windows.h>
#elif defined(__linux__)
    #define _MEMPOOL_PLATFORM_LINUX 1
    #include <sys/mman.h>
    #include <stdlib.h>
#elif defined(__APPLE__)
    #define _MEMPOOL_PLATFORM_MACOS 1
    #include <sys/mman.h>
    #include <stdlib.h>
#else
    #define _MEMPOOL_PLATFORM_OTHER 1
    #include <stdlib.h>
#endif

NAMESPACE_MEMORYPOOL_BEGIN

// 内存分配器: 接口与行为与 std::allocator 完全一致
// 自动检测平台, 使用该平台最快的内存分配 API
template<class _Ty>
class CMemoryPoolAllocator
{
public:
    using value_type      = _Ty;
    using pointer         = _Ty*;
    using const_pointer   = const _Ty*;
    using reference       = _Ty&;
    using const_reference = const _Ty&;
    using size_type       = size_t;
    using difference_type = ptrdiff_t;

    template<class _Other>
    struct rebind
    {
        using other = CMemoryPoolAllocator<_Other>;
    };

    CMemoryPoolAllocator() noexcept {}
    CMemoryPoolAllocator(const CMemoryPoolAllocator&) noexcept {}
    template<class _Other>
    CMemoryPoolAllocator(const CMemoryPoolAllocator<_Other>&) noexcept {}
    CMemoryPoolAllocator& operator=(const CMemoryPoolAllocator&) noexcept { return *this; }

    pointer address(reference _Val) const noexcept { return std::addressof(_Val); }
    const_pointer address(const_reference _Val) const noexcept { return std::addressof(_Val); }

    pointer allocate(size_type _Count, const void* _Hint = nullptr)
    {
#if defined(_MEMPOOL_PLATFORM_WINDOWS)
        void* p = VirtualAlloc(nullptr, _Count * sizeof(_Ty), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!p)
            throw std::bad_alloc();
        return static_cast<pointer>(p);
#elif defined(_MEMPOOL_PLATFORM_LINUX) || defined(_MEMPOOL_PLATFORM_MACOS)
        void* p = mmap(nullptr, _Count * sizeof(_Ty),
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS,
                        -1, 0);
        if (p == MAP_FAILED)
            throw std::bad_alloc();
        return static_cast<pointer>(p);
#else
        void* p = nullptr;
        if (posix_memalign(&p, sizeof(_Ty), _Count * sizeof(_Ty)) != 0)
            throw std::bad_alloc();
        return static_cast<pointer>(p);
#endif
    }

    void deallocate(pointer _Ptr, size_type)
    {
        if (!_Ptr)
            return;
#if defined(_MEMPOOL_PLATFORM_WINDOWS)
        VirtualFree(_Ptr, 0, MEM_RELEASE);
#elif defined(_MEMPOOL_PLATFORM_LINUX) || defined(_MEMPOOL_PLATFORM_MACOS)
        munmap(_Ptr, 0);
#else
        std::free(_Ptr);
#endif
    }

    size_type max_size() const noexcept
    {
        return static_cast<size_type>(-1) / sizeof(_Ty);
    }

    template <class _Objty, class... _Types>
    void construct(_Objty* const _Ptr, _Types&&... _Args)
    {
        ::new (const_cast<void*>(static_cast<const volatile void*>(_Ptr))) _Objty(std::forward<_Types>(_Args)...);
    }

    template<class _Uty>
    void destroy(_Uty* _Ptr)
    {
        _Ptr->~_Uty();
    }
};

NAMESPACE_MEMORYPOOL_END
