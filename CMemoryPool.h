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


    typedef struct LIST_NODE
    {
        LIST_NODE* next;     // 下一个节点
    }*PLIST_NODE;
    typedef struct MEMORY_HEAD
    {
        MEMORY_HEAD*    next;       // 下一个块分配的内存
        size_t          size;       // 这一块内存占用的字节
        PLIST_NODE      pFree;      // 回收回来的内存, 如果都没有回收, 那整个值就是0, 数组也分配完了就需要开辟新的内存块
        LPBYTE          arr;        // 分配出去的内存, 每次分配出去都指向下一个成员, 直到越界后就从链表中取下一个节点
    }*PMEMORY_HEAD;

    _Alloc              _Al;        // 分配器, 分配内存都是按字节分配, 自己计算分配成员需要多少字节 + 头部结构
    PMEMORY_HEAD        _Mem;       // 内存块头结构, 分配和回收都根据这个结构来操作
    PMEMORY_HEAD        _Now;       // 当前操作的内存块
    PMEMORY_HEAD        _Arr[16];   // 存放分配内存的内存块数组, 每个块分配的内存尺寸 = 4,8,12,16,20,24,28,32,40,48,56,64,80,96,112,128


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


