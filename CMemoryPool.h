#pragma once
#include "CMemoryAllocator.h"
NAMESPACE_MEMORYPOOL_BEGIN

#if CMEMORYPOOL_ISDEBUG == 0
template<class _Alloc = CMemoryPoolAllocator<BYTE>>
#endif
// �䳤�ڴ��, Ŀǰ�Ǵ����Զ����ڴ��Ϊ����, ÿ�η���4/8�ı����ֽ�
// 

// �Ȳ�Ū��, �п��ټ���
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


