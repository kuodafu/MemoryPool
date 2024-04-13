#pragma once
#include "CMemoryAllocator.h"
NAMESPACE_MEMORYPOOL_BEGIN

#if CMEMORYPOOL_ISDEBUG == 0
template<class _Alloc = CMemoryPoolAllocator<BYTE>>
#endif
// 变长内存池, 目前是打算以定长内存池为基础, 每次分配4/8的倍数字节
// 

// 先不弄了, 有空再继续
class CMemoryPool
{
#if CMEMORYPOOL_ISDEBUG
    using _Alloc = std::allocator<BYTE>;
#endif

private:


public:
    CMemoryPool()
    {

    }
    explicit CMemoryPool(size_t size) :CMemoryPool()
    {

    }
    CMemoryPool(const CMemoryPool& other) = delete;
    CMemoryPool& operator=(const CMemoryPool& other) = delete;

    CMemoryPool(CMemoryPool&& other) noexcept : CMemoryPool()
    {

    }

    ~CMemoryPool()
    {
    }

};


NAMESPACE_MEMORYPOOL_END


