#pragma once
#include "CMemoryAllocator.h"
NAMESPACE_MEMORYPOOL_BEGIN

// 定长内存池, 每次分配都是固定大小的内存
#if CMEMORYPOOL_ISDEBUG
using _Ty = uint8_t;
using _Alloc = CMemoryPoolAllocator;
#else
template<class _Ty = uint8_t, class _Alloc = CMemoryPoolAllocator<uint8_t>>
#endif
class CMemoryObjectPool
{
public:
    using value_type    = _Ty;
    using pointer       = _Ty*;
    using const_pointer = const _Ty*;

private:
    using byte_pointer = uint8_t*;

    // 内存块头
    typedef struct MEMORY_HEAD
    {
        MEMORY_HEAD*    next;       // 下一个内存块
        size_t          size;       // 这一块内存占用的总字节
        byte_pointer    item;       // 下一个要分配的地址
    }*PMEMORY_HEAD;

    // 每个槽位最少 sizeof(void*), 保证能存放下一个节点的指针
    static constexpr size_t SLOT_SIZE = sizeof(value_type) >= sizeof(void*) ? sizeof(value_type) : sizeof(void*);

    _Alloc              _Al;        // 分配器
    byte_pointer        _List;      // 全局回收链表头, 指向第一个空闲节点, nullptr 表示空链表
    PMEMORY_HEAD        _Mem;       // 内存块链表头
    PMEMORY_HEAD        _Now;       // 当前操作的内存块

public:
    CMemoryObjectPool() : _List(nullptr), _Mem(nullptr), _Al(), _Now(nullptr) {}
    explicit CMemoryObjectPool(size_t size) : _List(nullptr), _Mem(nullptr), _Al(), _Now(nullptr)
    {
        init(size);
    }
    CMemoryObjectPool(const CMemoryObjectPool& other) = delete;
    CMemoryObjectPool& operator=(const CMemoryObjectPool& other) = delete;

    CMemoryObjectPool(CMemoryObjectPool&& other) noexcept : _List(nullptr), _Mem(nullptr), _Al(), _Now(nullptr)
    {
        _Al = std::move(other._Al);
        _List = other._List;
        _Mem = other._Mem;
        _Now = other._Now;

        other._List = nullptr;
        other._Mem = nullptr;
        other._Now = nullptr;
    }
    ~CMemoryObjectPool()
    {
        Release();
    }

    // 释放所有内存
    inline void Release()
    {
        PMEMORY_HEAD node = _Mem;
        while (node)
        {
            PMEMORY_HEAD next = node->next;
            _Al.deallocate(reinterpret_cast<uint8_t*>(node), node->size);
            node = next;
        }
        _List = nullptr;
        _Mem = nullptr;
        _Now = nullptr;
    }

    // 清空内存池: 重置所有块到初始状态
    inline void clear()
    {
        if (!_Mem)
            return;
        _List = nullptr;
        _Now = _Mem;
        PMEMORY_HEAD pHead = _Mem;
        while (pHead)
        {
            pHead->item = reinterpret_cast<byte_pointer>(pHead) + sizeof(MEMORY_HEAD);
            pHead = pHead->next;
        }
    }

    // 当前内存池占用的总字节数
    inline size_t size() const
    {
        size_t total = 0;
        PMEMORY_HEAD p = _Mem;
        while (p)
        {
            total += p->size;
            p = p->next;
        }
        return total;
    }

    // 初始化内存池
    inline bool init(size_t count = 0x1000)
    {
        if (_Mem)
            return true;
        _Mem = malloc_head(count);
        _Now = _Mem;
        return true;
    }

    // 申请一个成员, 失败抛出 std::bad_alloc
    inline pointer malloc()
    {
        if (!_Mem)
            init();
        if (!_Mem)
            throw std::bad_alloc();

        // 第一步: 从全局回收链表分配
        if (_List)
        {
            byte_pointer p = _List;
            _List = *reinterpret_cast<byte_pointer*>(p);
            return reinterpret_cast<pointer>(p);
        }

        // 第二步: 从当前块分配
        byte_pointer pEnd = reinterpret_cast<byte_pointer>(_Now) + _Now->size;
        byte_pointer ptr = _Now->item;
        if (ptr + SLOT_SIZE > pEnd)
        {
            // 第三步: 所有块都不够, 申请新块
            size_t oldCount = (_Now->size - sizeof(MEMORY_HEAD)) / SLOT_SIZE;
            PMEMORY_HEAD pHead = malloc_head(oldCount * 2 + 1);

            _Now->next = pHead;
            _Now = pHead;
            ptr = _Now->item;
        }
        _Now->item += SLOT_SIZE;
        return reinterpret_cast<pointer>(ptr);
    }

    // 释放一个成员, 链入全局回收链表
    inline bool free(pointer p)
    {
        if (!p)
            return false;
        *reinterpret_cast<byte_pointer*>(p) = _List;
        _List = reinterpret_cast<byte_pointer>(p);
        return true;
    }

    // 查询地址是否属于内存池
    inline bool query(pointer p) const
    {
        return get_head(p) != nullptr;
    }

    // 输出当前内存池状态
    inline void dump() const
    {
        PMEMORY_HEAD pHead = _Mem;
        int index = 0;
        int freeCount = 0;
        byte_pointer f = _List;
        while (f)
        {
            freeCount++;
            f = *reinterpret_cast<byte_pointer*>(f);
        }
        while (pHead)
        {
            byte_pointer pAllocStart = reinterpret_cast<byte_pointer>(pHead) + sizeof(MEMORY_HEAD);
            int count = static_cast<int>((pHead->item - pAllocStart) / SLOT_SIZE);
            printf("%03d: 块地址 0x%p, 尺寸 %u, 已分配 %d 个, head = 0x%p\n",
                   index++, pHead, (uint32_t)pHead->size, count, (void*)pHead);
            pHead = pHead->next;
        }
        printf("空闲节点数: %d\n", freeCount);
    }

private:
    // 返回地址所属的内存块
    inline PMEMORY_HEAD get_head(const void* p) const
    {
        PMEMORY_HEAD pHead = _Mem;
        while (pHead)
        {
            byte_pointer pStart = reinterpret_cast<byte_pointer>(pHead) + sizeof(MEMORY_HEAD);
            byte_pointer pEnd = reinterpret_cast<byte_pointer>(pHead) + pHead->size;
            if (reinterpret_cast<byte_pointer>(const_cast<void*>(p)) >= pStart &&
                reinterpret_cast<byte_pointer>(const_cast<void*>(p)) < pEnd)
            {
                return pHead;
            }
            pHead = pHead->next;
        }
        return nullptr;
    }

    // 分配一块内存
    inline PMEMORY_HEAD malloc_head(size_t count)
    {
        auto totalSize = count * SLOT_SIZE;
        auto pStart = reinterpret_cast<byte_pointer>(_Al.allocate(totalSize));

        PMEMORY_HEAD pHead = reinterpret_cast<PMEMORY_HEAD>(pStart);
        pHead->next = nullptr;
        pHead->size = totalSize;
        pHead->item = pStart + sizeof(MEMORY_HEAD);
        return pHead;
    }

    // 扩充: 在链表尾部追加一个新块
    inline PMEMORY_HEAD expand(PMEMORY_HEAD pTail)
    {
        if (!pTail)
            return nullptr;
        size_t oldCount = (pTail->size - sizeof(MEMORY_HEAD)) / SLOT_SIZE;
        PMEMORY_HEAD pNew = malloc_head(oldCount * 2 + 1);
        pTail->next = pNew;
        return pNew;
    }
};

NAMESPACE_MEMORYPOOL_END
