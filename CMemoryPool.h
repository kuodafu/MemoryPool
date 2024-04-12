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


    typedef struct LIST_NODE
    {
        LIST_NODE* next;     // ��һ���ڵ�
    }*PLIST_NODE;
    typedef struct MEMORY_HEAD
    {
        MEMORY_HEAD*    next;       // ��һ���������ڴ�
        size_t          size;       // ��һ���ڴ�ռ�õ��ֽ�
        PLIST_NODE      pFree;      // ���ջ������ڴ�, �����û�л���, ������ֵ����0, ����Ҳ�������˾���Ҫ�����µ��ڴ��
        LPBYTE          arr;        // �����ȥ���ڴ�, ÿ�η����ȥ��ָ����һ����Ա, ֱ��Խ���ʹ�������ȡ��һ���ڵ�
    }*PMEMORY_HEAD;

    _Alloc              _Al;        // ������, �����ڴ涼�ǰ��ֽڷ���, �Լ���������Ա��Ҫ�����ֽ� + ͷ���ṹ
    PMEMORY_HEAD        _Mem;       // �ڴ��ͷ�ṹ, ����ͻ��ն���������ṹ������
    PMEMORY_HEAD        _Now;       // ��ǰ�������ڴ��
    PMEMORY_HEAD        _Arr[16];   // ��ŷ����ڴ���ڴ������, ÿ���������ڴ�ߴ� = 4,8,12,16,20,24,28,32,40,48,56,64,80,96,112,128


public:
    CMemoryPool() :_Mem(0), _Al(), _Now(0), _Arr{ 0 }
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


