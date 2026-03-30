#pragma once
#include "CMemoryAllocator.h"
#include <cassert>
#include <type_traits>
#include <new>
#include <cstring>

NAMESPACE_MEMORYPOOL_BEGIN

template<class _Alloc = CMemoryPoolAllocator<uint8_t>>
class CMemoryPoolBase
{
protected:

    using byte_pointer = uint8_t*;


    typedef struct LIST_NODE { LIST_NODE* next; }*PLIST_NODE;

    typedef struct MEMORY_HEAD
    {
        MEMORY_HEAD*    next;       // 下一个内存块
        size_t          size;       // 这一块内存占用的总字节 (已对齐到 0x1000)
        byte_pointer    item;       // 下一个要分配的地址
        PLIST_NODE      freeList;   // 本块的空闲链表头, nullptr 表示空
        size_t          freeCount;  // 本块当前空闲节点数 (在空闲链表里的节点数)
    }*PMEMORY_HEAD;

    size_t              SLOT_SIZE;  // 运行时决定, 每次分配多大字节
    _Alloc              _Al;        // 分配器
    PMEMORY_HEAD        _Mem;       // 内存块链表头
    PMEMORY_HEAD        _Now;       // 当前操作的内存块

public:

    explicit CMemoryPoolBase(size_t slotSize, size_t count)
        : SLOT_SIZE(align_slot(slotSize))
        , _Al()
        , _Mem(nullptr)
        , _Now(nullptr)
    {
        if (count > 0)
            init(count);
    }


    CMemoryPoolBase(const CMemoryPoolBase& other) = delete;
    CMemoryPoolBase& operator=(const CMemoryPoolBase& other) = delete;

    CMemoryPoolBase(CMemoryPoolBase&& other) noexcept
        : SLOT_SIZE(other.SLOT_SIZE)
        , _Al(std::move(other._Al))
        , _Mem(other._Mem)
        , _Now(other._Now)
    {
        other._Mem = nullptr;
        other._Now = nullptr;
    }

    /**
     * @brief 初始化内存池
     * @param count 初始槽位数量, 默认 4096, 仅首次初始化时生效
     * @return true 初始化成功或已经初始化过; false 初始化失败
     */
    inline bool init(size_t count = 0x1000)
    {
        if (_Mem)
            return true;
        _Mem = malloc_head(count);
        _Now = _Mem;
        return true;
    }

    /**
    * @brief 查询地址是否属于本内存池
    * @param p 要查询的指针
    * @return true 指针属于本内存池且对齐到槽位边界; false 指针不属于本内存池或未对齐
    * @note debug 模式下会断言地址是否对齐到槽位边界
    * @note 热点优先: 先检查当前块 _Now, 大多数指针是最近分配的
    */
    inline bool query(void* p) const
    {
        byte_pointer ptr = reinterpret_cast<byte_pointer>(p);

        // 热点优先: 先检查当前块
        if (_Now)
        {
            byte_pointer pStart = reinterpret_cast<byte_pointer>(_Now) + sizeof(MEMORY_HEAD);
            byte_pointer pEnd = reinterpret_cast<byte_pointer>(_Now) + _Now->size;
            if (ptr >= pStart && ptr < pEnd)
            {
                assert((ptr - pStart) % SLOT_SIZE == 0 && "query address is not aligned to slot boundary");
                return true;
            }
        }

        // 不在当前块, 遍历其他块
        PMEMORY_HEAD pHead = _Mem;
        while (pHead)
        {
            if (pHead != _Now)
            {
                byte_pointer pStart = reinterpret_cast<byte_pointer>(pHead) + sizeof(MEMORY_HEAD);
                byte_pointer pEnd = reinterpret_cast<byte_pointer>(pHead) + pHead->size;
                if (ptr >= pStart && ptr < pEnd)
                {
                    assert((ptr - pStart) % SLOT_SIZE == 0 && "query address is not aligned to slot boundary");
                    return true;
                }
            }
            pHead = pHead->next;
        }
        return false;
    }

    /**
     * @brief 释放一块内存
     * @param p 要释放的指针
     * @return true 释放成功; false 指针不属于本内存池
     * @note 先检查所有权, 再调用子类 hook, 最后归还到块链表
     */
    bool free(void* p)
    {
        if (!p)
            return false;

        byte_pointer ptr = reinterpret_cast<byte_pointer>(p);

        // 热点优先: 先检查当前块
        if (try_free_block(_Now, ptr))
            return true;
        

        // 当前块不匹配, 遍历其他块
        PMEMORY_HEAD pHead = _Mem;
        while (pHead)
        {
            if (pHead != _Now && try_free_block(pHead, ptr))
                return true;
            pHead = pHead->next;
        }

        return false;
    }

    /**
     * @brief 清空内存池
     * @note 重置所有内存块到初始状态, 内存块可继续使用。
     *       子类覆盖 _destroy_block() 以在清空前析构对象。
     */
    inline void clear()
    {
        if (!_Mem)
            return;
        _Now = _Mem;
        PMEMORY_HEAD pHead = _Mem;
        while (pHead)
        {
            _destroy_block(pHead);
            _reset_block(pHead);
            pHead = pHead->next;
        }
    }

    /**
     * @brief 销毁内存池, 释放所有内存回 OS
     * @note 先对所有块析构对象, 再逐块归还内存。
     *       子类覆盖 _destroy_block() 以析构对象。
     */
    inline void destroy()
    {
        PMEMORY_HEAD node = _Mem;
        while (node)
        {
            _destroy_block(node);
            PMEMORY_HEAD next = node->next;
            _deallocate_block(node);
            node = next;
        }
        _Mem = nullptr;
        _Now = nullptr;
    }

protected:
    //------------------------------------------------------------
    // 释放前的钩子, 由子类决定要不要析构对象
    //   - CMemoryBytePool: 空实现(不需要析构)
    //   - CMemoryObjectPool: 调用对象的析构函数
    //------------------------------------------------------------
    virtual void _before_free(void* p) {}

    //------------------------------------------------------------
    // 销毁块内所有已分配对象, 由子类实现析构逻辑
    //   - CMemoryBytePool: 空实现
    //   - CMemoryObjectPool: 遍历活跃槽位, 调用析构函数
    //------------------------------------------------------------
    virtual void _destroy_block(PMEMORY_HEAD pHead) {}

    //------------------------------------------------------------
    // 重置单个块到初始状态, 供 clear() 调用
    //------------------------------------------------------------
    virtual void _reset_block(PMEMORY_HEAD pHead)
    {
        pHead->item = reinterpret_cast<byte_pointer>(pHead) + sizeof(MEMORY_HEAD);
        pHead->freeList = nullptr;
        pHead->freeCount = 0;
    }

    //------------------------------------------------------------
    // 归还单个块内存给 OS, 供 destroy() 调用
    //------------------------------------------------------------
    virtual void _deallocate_block(PMEMORY_HEAD pHead)
    {
        _Al.deallocate(reinterpret_cast<byte_pointer>(pHead), pHead->size);
    }

    //------------------------------------------------------------
    // 重新设置槽位尺寸
    // @param slotSize 新的槽位尺寸,已对齐到 sizeof(void*)
    // @exception std::runtime_error 如果池中仍有未释放的内存则抛出
    // @note 调用前必须确保池为空(所有块 item 回到起始位置)
    //------------------------------------------------------------
    inline void resize_slot(size_t slotSize)
    {
        if (!is_empty())
            throw std::runtime_error("resize_slot: 池中仍有未释放的内存,无法改变槽位尺寸");
        SLOT_SIZE = align_slot(slotSize);
    }

    /**
     * @brief 查询池是否为空(所有块均无分配)
     * @return true 池为空,无任何已分配未释放的内存;false 池中仍有内存未释放
     */
    inline bool is_empty() const
    {
        PMEMORY_HEAD pHead = _Mem;
        while (pHead)
        {
            byte_pointer pStart = reinterpret_cast<byte_pointer>(pHead) + sizeof(MEMORY_HEAD);
            if (pHead->item != pStart)
                return false;
            pHead = pHead->next;
        }
        return true;
    }

    /**
     * @brief 合并实现:将 pHead 链表追加到本池,并按内存块尺寸从小到大排序
     * @param pHead 对方池的内存块链表头
     * @note 由子类的 merge() 调用,子类只负责转发
     */
    inline void merge(PMEMORY_HEAD pHead)
    {
        if (_Mem == nullptr)
        {
            _Mem = pHead;
        }
        else
        {
            // 找到本池末尾,接上对方链表
            PMEMORY_HEAD pTail = _Mem;
            while (pTail->next)
                pTail = pTail->next;
            pTail->next = pHead;
        }
        // 按内存块尺寸从小到大排序
        _sort_by_size();
        _Now = _Mem;
    }

    //------------------------------------------------------------
    // 按内存块尺寸从小到大排序(插入排序)
    //------------------------------------------------------------
    inline void _sort_by_size()
    {
        if (!_Mem || !_Mem->next)
            return;

        PMEMORY_HEAD head = _Mem;
        PMEMORY_HEAD sorted = nullptr;

        while (head)
        {
            PMEMORY_HEAD next = head->next;

            if (!sorted || head->size <= sorted->size)
            {
                head->next = sorted;
                sorted = head;
            }
            else
            {
                PMEMORY_HEAD cur = sorted;
                while (cur->next && cur->next->size < head->size)
                    cur = cur->next;
                head->next = cur->next;
                cur->next = head;
            }
            head = next;
        }
        _Mem = sorted;
    }


public:
    /**
     * @brief 获取内存池占用的总字节数
     * @return 总字节数 (包含每块头部的开销)
     */
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

    /**
     * @brief 输出调试状态
     * @note 打印内存池的详细状态到标准输出, 包含每个内存块的信息和空闲节点统计
     */
    inline void dump() const
    {
        PMEMORY_HEAD pHead = _Mem;
        int index = 0;
        int totalFreeCount = 0;
        while (pHead)
        {
            byte_pointer pAllocStart = reinterpret_cast<byte_pointer>(pHead) + sizeof(MEMORY_HEAD);
            int count = static_cast<int>((pHead->item - pAllocStart) / SLOT_SIZE);

            totalFreeCount += static_cast<int>(pHead->freeCount);

            printf("%03d: 块地址 0x%p, 尺寸 %u, 已分配 %d 个, 空闲 %d 个, head = 0x%p\n",
                   index++, pHead, (uint32_t)pHead->size, count, (int)pHead->freeCount, (void*)pHead);
            pHead = pHead->next;
        }
        printf("总空闲节点数: %d\n", totalFreeCount);
    }


protected:

    //------------------------------------------------------------
    // 裸分配
    //
    // 实现: 分配一个槽位的裸内存, 不调用构造函数
    //
    // 分配顺序 (优先级从高到低):
    //   1. 从 _Now (当前块) 末尾 bump 分配 (最快路径)
    //   2. 从 _Now 的 free list 分配
    //   3. 遍历其他所有块, 先尝试 bump 分配, 再尝试 free list
    //   4. 所有块都不够, 申请一个新块, 大小翻倍 + 1
    //------------------------------------------------------------
    void* alloc()
    {
        if (!_Mem)
            init();
        if (!_Mem)
            throw std::bad_alloc();

        // 1. 从当前块末尾分配 (bump allocate, 最快路径)
        byte_pointer pEnd = reinterpret_cast<byte_pointer>(_Now) + _Now->size;
        byte_pointer ptr = _Now->item;
        if (ptr + SLOT_SIZE <= pEnd)
        {
            _Now->item += SLOT_SIZE;
            return ptr;
        }

        // 2. 从当前块的 free list 分配 (缓存亲和)
        if (_Now->freeList)
        {
            PLIST_NODE pNode = _Now->freeList;
            _Now->freeList = pNode->next;
            _Now->freeCount--;
            return pNode;
        }

        // 3. 遍历其他块: 先尝试 bump 分配 (缓存亲和), 再尝试 free list
        PMEMORY_HEAD pHead = _Mem;
        while (pHead)
        {
            if (pHead != _Now)
            {
                byte_pointer pBlockEnd = reinterpret_cast<byte_pointer>(pHead) + pHead->size;
                byte_pointer pBlockItem = pHead->item;

                // 优先 bump 分配, 比 free list 更有缓存亲和
                if (pBlockItem + SLOT_SIZE <= pBlockEnd)
                {
                    pHead->item = pBlockItem + SLOT_SIZE;
                    _Now = pHead;   // 下次分配从这个内存块里分配
                    return pBlockItem;
                }

                // bump 不够, 尝试 free list
                if (pHead->freeList)
                {
                    PLIST_NODE pNode = pHead->freeList;
                    pHead->freeList = pNode->next;
                    pHead->freeCount--;
                    _Now = pHead;   // 下次分配从这个内存块里分配
                    return pNode;
                }
            }
            pHead = pHead->next;
        }

        // 4. 所有块都不够, 申请新块
        size_t oldCount = (_Now->size - sizeof(MEMORY_HEAD)) / SLOT_SIZE;
        PMEMORY_HEAD pNew = malloc_head(oldCount * 2 + 1);

        _Now->next = pNew;
        _Now = pNew;
        ptr = _Now->item;

        _Now->item += SLOT_SIZE;
        return ptr;
    }

    //------------------------------------------------------------
    // 尝试将 ptr 释放到指定内存块
    //
    // 实现:
    //   1. 范围检查: ptr 是否落在 [pStart, pAllocEnd) 内
    //   2. 对齐修正: 计算 ptr 对应槽位的起始地址
    //   3. bump pop: 如果是最后分配的槽位 (pAligned + SLOT_SIZE == item), 直接回退 item
    //   4. 入链表: 链入本块的 free list 头部, freeCount++
    //   5. 全块空闲判断: item 回到起点且 freeCount > 0 时, 整块重置到 bump 模式
    //------------------------------------------------------------
    bool try_free_block(PMEMORY_HEAD pHead, byte_pointer ptr)
    {
        byte_pointer pStart = reinterpret_cast<byte_pointer>(pHead) + sizeof(MEMORY_HEAD);
        byte_pointer pAllocEnd = pHead->item;

        if (ptr < pStart || ptr >= pAllocEnd)
            return false;

        _before_free(ptr);


        // 计算对齐后的槽位地址
        size_t offset = static_cast<size_t>(ptr - pStart);
        size_t alignedOffset = offset / SLOT_SIZE * SLOT_SIZE;
        byte_pointer pAligned = pStart + alignedOffset;

        // 未对齐: _DEBUG 断言, release 自动修正
        assert((ptr - pStart) % SLOT_SIZE == 0 && "free address is not aligned to slot boundary");

        // 如果正好是最后一个分配的槽位, 直接回退 item (bump pop)
        if (pAligned + SLOT_SIZE == pHead->item)
        {
            pHead->item -= SLOT_SIZE;
            return true;
        }

        // 否则加入本块的 free list
        pHead->freeCount++;

        // 如果整块都空闲了, 重置到 bump 模式
        // 条件: 空闲槽位数 == 已分配的槽位数
        size_t allocatedSlots = static_cast<size_t>(pHead->item - pStart) / SLOT_SIZE;
        if (allocatedSlots == pHead->freeCount)
        {
            pHead->item = pStart;
            pHead->freeList = nullptr;
            pHead->freeCount = 0;
        }
        else
        {
            auto pNode = reinterpret_cast<PLIST_NODE>(pAligned);
            pNode->next = pHead->freeList;
            pHead->freeList = pNode;
        }
        return true;
    }

    //------------------------------------------------------------
    // 分配一块内存
    //
    // 实现: 向底层分配器申请一块连续内存, 大小对齐到 0x1000
    //   +---------------------------+ <- 对齐到 0x1000
    //   | MEMORY_HEAD               |  sizeof(MEMORY_HEAD) 字节
    //   |---------------------------|
    //   | 槽位 0  | 槽位 1 | ...     |  count * SLOT_SIZE 字节
    //   +---------------------------+
    //------------------------------------------------------------
    inline PMEMORY_HEAD malloc_head(size_t count)
    {
        auto dataSize = count * SLOT_SIZE;
        auto totalSize = dataSize + sizeof(MEMORY_HEAD);
        totalSize = (totalSize + 0xFFF) & ~static_cast<size_t>(0xFFF);
        auto pStart = reinterpret_cast<byte_pointer>(_Al.allocate(totalSize));

        auto pHead = reinterpret_cast<PMEMORY_HEAD>(pStart);
        pHead->next = nullptr;
        pHead->size = totalSize;
        pHead->item = pStart + sizeof(MEMORY_HEAD);
        pHead->freeList = nullptr;
        pHead->freeCount = 0;
        return pHead;
    }

    static size_t align_slot(size_t slotSize)
    {
        size_t mask = sizeof(void*) - 1;
        return (slotSize + mask) & ~mask;
    }

};

NAMESPACE_MEMORYPOOL_END
