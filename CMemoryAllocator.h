#pragma once
#define CMEMORYPOOL_ISDEBUG 1

#define NAMESPACE_MEMORYPOOL_BEGIN  namespace kuodafu {
#define NAMESPACE_MEMORYPOOL_END    }


NAMESPACE_MEMORYPOOL_BEGIN

#if CMEMORYPOOL_ISDEBUG
typedef int _Ty;
#else
template<class _Ty = LPVOID, class _Alloc = std::allocator<_Ty>>
#endif
class CMemoryPoolAllocator : public std::allocator<BYTE>
{

public:

    void deallocate(_Ty* const _Ptr, const size_t _Count) {
        _STL_ASSERT(_Ptr != nullptr || _Count == 0, "null pointer cannot point to a block of non-zero size");
        if (_Ptr)
            VirtualFree(_Ptr, 0, MEM_RELEASE);
        // no overflow check on the following multiply; we assume _Allocate did that check

    }

    // 内存池使用的内存分配函数, 参数是分配多少页
    __declspec(allocator) _Ty* allocate(_CRT_GUARDOVERFLOW const size_t _Count) {
        static_assert(sizeof(value_type) > 0, "value_type must be complete before calling allocate.");
        for (;;)
        {
            if (void* const block = VirtualAlloc(0, _Count, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))
            {
                return reinterpret_cast<_Ty*>(block);
            }

            throw std::bad_alloc();
        }

        return 0;
    }

};





NAMESPACE_MEMORYPOOL_END
