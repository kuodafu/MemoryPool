#pragma once
#include "CMemoryAllocator.h"
#include <cassert>
#include <type_traits>
#include <new>
#include <cstring>
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

    typedef struct LIST_NODE
    {
        LIST_NODE*      next;       // 下一个节点
    }*PLIST_NODE;

    // 内存块头
    typedef struct MEMORY_HEAD
    {
        MEMORY_HEAD*    next;       // 下一个内存块
        size_t          size;       // 这一块内存占用的总字节 (已对齐到 0x1000)
        byte_pointer    item;       // 下一个要分配的地址
        PLIST_NODE      freeList;   // 本块的空闲链表头, nullptr 表示空
        size_t          freeCount;  // 本块当前空闲节点数 (在空闲链表里的节点数)
    }*PMEMORY_HEAD;

    // 每个槽位最少 sizeof(void*), 保证能存放下一个节点的指针
    static constexpr size_t SLOT_SIZE = sizeof(value_type) >= sizeof(void*) ? sizeof(value_type) : sizeof(void*);

    _Alloc              _Al;        // 分配器
    PMEMORY_HEAD        _Mem;       // 内存块链表头
    PMEMORY_HEAD        _Now;       // 当前操作的内存块

public:
    /**
     * @brief 构造一个空的内存池, 延迟分配内存
     */
    CMemoryObjectPool() : _Mem(nullptr), _Al(), _Now(nullptr) {}

    /**
     * @brief 带初始容量的构造函数
     * @param count 初始槽位数量, 默认 4096
     */
    explicit CMemoryObjectPool(size_t count) : _Mem(nullptr), _Al(), _Now(nullptr)
    {
        init(count);
    }

    CMemoryObjectPool(const CMemoryObjectPool& other) = delete;
    CMemoryObjectPool& operator=(const CMemoryObjectPool& other) = delete;

    /**
     * @brief 移动构造函数
     * @param other 被移动的内存池对象
     */
    CMemoryObjectPool(CMemoryObjectPool&& other) noexcept : _Mem(nullptr), _Al(), _Now(nullptr)
    {
        _Al = std::move(other._Al);
        _Mem = other._Mem;
        _Now = other._Now;

        other._Mem = nullptr;
        other._Now = nullptr;
    }

    /**
     * @brief 析构函数, 自动释放所有内存
     */
    ~CMemoryObjectPool()
    {
        Release();
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
     * @brief 申请一个对象
     * @tparam _Args 构造函数的参数类型列表
     * @param ..._Arg 构造函数的参数, 无参时调用默认构造函数
     *        支持: pool.malloc() / pool.malloc(arg1) / pool.malloc(arg1, arg2) 等
     * @return 指向新分配对象的指针
     * @exception std::bad_alloc 分配失败时抛出
     */
    template<typename... _Args>
    inline pointer malloc(_Args&&... _Arg)
    {
        if (!_Mem)
            init();
        if (!_Mem)
            throw std::bad_alloc();

        pointer p = alloc();
        construct(p, std::forward<_Args>(_Arg)...);
        return p;
    }

    /**
     * @brief 释放一个对象
     * @param p 指向要释放的对象指针, 不能为 nullptr, 必须是由本内存池分配的指针
     * @return true 释放成功; false 释放失败 (p 为 nullptr 或指针不属于本内存池)
     * @note 如果传入的地址未对齐到槽位边界:
     *       _DEBUG 模式触发 assert 断言失败
     *       release 模式自动对齐到正确的槽位地址
     */
    inline bool free(pointer p)
    {
        if (!p)
            return false;

        destroy(p);
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
     * @brief 释放所有内存
     * @note 如果类型需要析构函数，则依次对每个内存块中已分配的所有对象调用析构函数，
     *       然后释放内存回 OS。
     */
    inline void Release()
    {
        PMEMORY_HEAD node = _Mem;
        while (node)
        {
            _destroy_block(node);
            PMEMORY_HEAD next = node->next;
            _Al.deallocate(reinterpret_cast<uint8_t*>(node), node->size);
            node = next;
        }
        _Mem = nullptr;
        _Now = nullptr;
    }

    /**
     * @brief 清空内存池
     * @note 重置所有内存块到初始状态。如果类型需要析构函数，则依次对每个内存块中
     *       已分配的所有对象调用析构函数，然后清空空闲链表，内存块可继续使用。
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
            pHead->item = reinterpret_cast<byte_pointer>(pHead) + sizeof(MEMORY_HEAD);
            pHead->freeList = nullptr;
            pHead->freeCount = 0;
            pHead = pHead->next;
        }
    }

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
     * @brief 查询地址是否属于本内存池
     * @param p 要查询的指针
     * @return true 指针属于本内存池且对齐到槽位边界; false 指针不属于本内存池或未对齐
     * @note debug 模式下会断言地址是否对齐到槽位边界
     * @note 热点优先: 先检查当前块 _Now，大多数指针是最近分配的
     */
    inline bool query(pointer p) const
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

        // 不在当前块，遍历其他块
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


private:
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
    inline pointer alloc()
    {
        // 1. 从当前块末尾分配 (bump allocate, 最快路径)
        byte_pointer pEnd = reinterpret_cast<byte_pointer>(_Now) + _Now->size;
        byte_pointer ptr = _Now->item;
        if (ptr + SLOT_SIZE <= pEnd)
        {
            _Now->item += SLOT_SIZE;
            return reinterpret_cast<pointer>(ptr);
        }

        // 2. 从当前块的 free list 分配 (缓存亲和)
        if (_Now->freeList)
        {
            PLIST_NODE pNode = _Now->freeList;
            _Now->freeList = pNode->next;
            _Now->freeCount--;
            return reinterpret_cast<pointer>(pNode);
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
                    return reinterpret_cast<pointer>(pBlockItem);
                }

                // bump 不够, 尝试 free list
                if (pHead->freeList)
                {
                    PLIST_NODE pNode = pHead->freeList;
                    pHead->freeList = pNode->next;
                    pHead->freeCount--;
                    return reinterpret_cast<pointer>(pNode);
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
        return reinterpret_cast<pointer>(ptr);
    }

    //------------------------------------------------------------
    // 构造对象
    //
    // 实现: 使用 SFINAE 编译期分支
    //   - 有构造函数的类型: 调用 placement new, 参数完美转发
    //   - 无构造函数的类型 (int, double, PAINTSTRUCT 等): 空操作, 参数被忽略
    //------------------------------------------------------------
    template<typename... _Types>
    inline typename std::enable_if<sizeof...( _Types) == 0 || !std::is_trivially_default_constructible<_Ty>::value, void>::type
    construct(pointer _Ptr, _Types&&... _Args)
    {
        ::new (const_cast<void*>(static_cast<const volatile void*>(_Ptr))) _Ty(std::forward<_Types>(_Args)...);
    }

    template<typename... _Types>
    inline typename std::enable_if<sizeof...( _Types) != 0 && std::is_trivially_default_constructible<_Ty>::value, void>::type
    construct(pointer, _Types&&...) {}

    //------------------------------------------------------------
    // 析构对象
    //
    // 实现: 使用 SFINAE 编译期分支
    //   - 有析构函数的类型: 调用 p->~_Ty()
    //   - 无析构函数的类型 (int, double, PAINTSTRUCT 等): 空操作
    //------------------------------------------------------------
    template<int _IsTriviallyDestructible = std::is_trivially_destructible<_Ty>::value>
    inline typename std::enable_if<_IsTriviallyDestructible, void>::type
    destroy(pointer) {}

    template<int _IsTriviallyDestructible = std::is_trivially_destructible<_Ty>::value>
    inline typename std::enable_if<!_IsTriviallyDestructible, void>::type
    destroy(pointer p)
    {
        p->~_Ty();
    }

    //------------------------------------------------------------
    // 销毁一块内存块内的所有已分配对象
    //
    // 实现: SFINAE 编译期分支
    //   - trivially destructible: 空函数体，零开销
    //   - 否则:
    //       1. 计算 [pStart, item) 共有多少个槽位 N
    //       2. 分配位图：小块用栈上缓冲区；大块优先借用本块末尾空闲空间；空间仍不够才动态 new
    //       3. 遍历 free list，把对应 bit 置 1（表示已 free，不需析构）
    //       4. 遍历 [pStart, item)，只对 bit = 0 的槽位调用析构函数
    //------------------------------------------------------------
    template<int _IsTriviallyDestructible = std::is_trivially_destructible<_Ty>::value>
    inline typename std::enable_if<_IsTriviallyDestructible, void>::type
    _destroy_block(PMEMORY_HEAD) {}

    template<int _IsTriviallyDestructible = std::is_trivially_destructible<_Ty>::value>
    inline typename std::enable_if<!_IsTriviallyDestructible, void>::type
    _destroy_block(PMEMORY_HEAD pHead)
    {
        byte_pointer pStart = reinterpret_cast<byte_pointer>(pHead) + sizeof(MEMORY_HEAD);
        byte_pointer pEnd = pHead->item;

        // 计算 [pStart, item) 区间共有多少个槽位
        size_t totalSlots = static_cast<size_t>(pEnd - pStart) / SLOT_SIZE;
        if (totalSlots == 0)
            return;

        // 如果回收链表是空的, 那就是start 到 item全部的槽位都要调用析构函数
        if (!pHead->freeList)
        {
            for (byte_pointer p = pStart; p < pEnd; p += SLOT_SIZE)
                destroy(reinterpret_cast<pointer>(p));
            return;
        }

        size_t bitmapWords = (totalSlots + 31) >> 5;

        // 栈上局部位图缓冲区，覆盖小块场景；大块借用本块末尾的空闲内存
        static constexpr size_t STACK_BITMAP_SIZE = 0x1000;
        uint32_t stackBitmap[STACK_BITMAP_SIZE];

        // 小块用栈上缓冲区，大块借用本块末尾剩余空间（item 之后的区域）
        uint32_t* bitmap = nullptr;
        bool needFree = false;
        size_t bitmapBytes = bitmapWords * sizeof(uint32_t);
        if (bitmapWords <= STACK_BITMAP_SIZE)
        {
            bitmap = stackBitmap;
        }
        else
        {
            byte_pointer blockEnd = reinterpret_cast<byte_pointer>(pHead) + pHead->size;
            byte_pointer freeStart = pEnd;
            size_t freeBytes = static_cast<size_t>(blockEnd - freeStart);

            if (freeBytes >= bitmapBytes)
            {
                // 块末尾有足够空闲空间，借用之
                bitmap = reinterpret_cast<uint32_t*>(freeStart);
            }
            else
            {
                // 空间不够，动态分配
                bitmap = new uint32_t[bitmapWords];
                needFree = true;
            }
        }

        std::memset(bitmap, 0, bitmapBytes);

        // 遍历 free list，标记已释放的槽位
        PLIST_NODE pNode = pHead->freeList;
        while (pNode)
        {
            size_t idx = (reinterpret_cast<byte_pointer>(pNode) - pStart) / SLOT_SIZE;
            bitmap[idx >> 5] |= 1U << (idx & 31);
            pNode = pNode->next;
        }

        // 遍历所有槽位，对未标记的（活跃的）调用析构函数
        for (size_t i = 0; i < totalSlots; ++i)
        {
            if ((bitmap[i >> 5] & (1U << (i & 31))) == 0)
            {
                destroy(reinterpret_cast<pointer>(pStart + i * SLOT_SIZE));
            }
        }

        if (needFree)
            delete[] bitmap;
    }

    //------------------------------------------------------------
    // 尝试将 ptr 释放到指定内存块
    //
    // 实现:
    //   1. 范围检查: ptr 是否落在 [pStart, pAllocEnd) 内
    //   2. 对齐修正: 计算 ptr 对应槽位的起始地址
    //   3. bump pop: 如果是最后分配的槽位 (pAligned + SLOT_SIZE == item), 直接回退 item
    //   4. 入链表: 链入本块的 free list 头部, freeCount++
    //   5. 全块空闲判断: item 回到起点且 freeCount > 0 时，整块重置到 bump 模式
    //------------------------------------------------------------
    inline bool try_free_block(PMEMORY_HEAD pHead, byte_pointer ptr)
    {
        byte_pointer pStart = reinterpret_cast<byte_pointer>(pHead) + sizeof(MEMORY_HEAD);
        byte_pointer pAllocEnd = pHead->item;

        if (ptr < pStart || ptr >= pAllocEnd)
            return false;

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
};

NAMESPACE_MEMORYPOOL_END
