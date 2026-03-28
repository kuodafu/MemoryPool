#pragma once
#include "CMemoryAllocator.h"
NAMESPACE_MEMORYPOOL_BEGIN

// 定长内存池, 每次分配都是固定大小的内存
#if CMEMORYPOOL_ISDEBUG
using _Ty = uint8_t;
#else
template<class _Ty = LPVOID, class _Alloc = std::allocator<uint8_t>>
#endif
class CMemoryObjectPool
{
public:
    using value_type    = _Ty;
    using pointer       = _Ty*;
    using const_pointer = const _Ty*;
private:
#if CMEMORYPOOL_ISDEBUG
    using _Alloc = std::allocator<value_type>;
#endif

    using byte_pointer          = uint8_t*;
    using const_byte_pointer    = const uint8_t*;

    typedef struct MEMORY_HEAD
    {
        MEMORY_HEAD*    next;           // 下一个内存块
        size_t          size;           // 这一块内存占用的字节
        byte_pointer    item;           // 下一个要分配的地址
        uint32_t        head;           // 回收链表头, 存的是槽位相对于 pStart 的偏移量, 0 表示空链表
    }*PMEMORY_HEAD;

    // free 节点: 只存 next 偏移量 (4字节)
    // next = 0 表示链表结束
    typedef uint32_t FREE_NODE;

    // 每个槽位大小: 取 value_type 和 FREE_NODE 的较大值
    static constexpr size_t SLOT_SIZE = sizeof(value_type) >= sizeof(FREE_NODE) ? sizeof(value_type) : sizeof(FREE_NODE);

    // 槽位足够存放下一个内存块指针时, 用指针直接定位块 (O(1) free)
    static constexpr bool USES_BLOCK_PTR = SLOT_SIZE >= sizeof(PMEMORY_HEAD);

    _Alloc              _Al;        // 分配器
    PMEMORY_HEAD        _Mem;       // 内存块链表头
    PMEMORY_HEAD        _Now;       // 当前操作的内存块

public:
    CMemoryObjectPool() :_Mem(nullptr), _Al(), _Now(nullptr)
    {

    }
    explicit CMemoryObjectPool(size_t size) :_Mem(nullptr), _Al(), _Now(nullptr)
    {
        init(size);
    }
    CMemoryObjectPool(const CMemoryObjectPool& other) = delete;
    CMemoryObjectPool& operator=(const CMemoryObjectPool& other) = delete;

    CMemoryObjectPool(CMemoryObjectPool&& other) noexcept :_Mem(nullptr), _Al(), _Now(nullptr)
    {
        _Al = std::move(other._Al);
        _Mem = other._Mem;
        _Now = other._Now;

        other._Mem = nullptr;
        other._Now = nullptr;
    }
    ~CMemoryObjectPool()
    {
        Release();
    }
    inline void Release()
    {
        PMEMORY_HEAD node = _Mem;
        while (node)
        {
            PMEMORY_HEAD next = node->next;
            _Al.deallocate(reinterpret_cast<uint8_t*>(node), node->size);
            node = next;
        }
        _Mem = nullptr;
        _Now = nullptr;
    }
    // 清空内存池, 恢复到初始状态
    inline void clear()
    {
        if (!_Mem)
            return;

        PMEMORY_HEAD pHead = _Mem;
        _Now = _Mem;
        while (pHead)
        {
            pHead->item = reinterpret_cast<byte_pointer>(pHead) + sizeof(MEMORY_HEAD);
            pHead->head = 0;
            pHead = pHead->next;
        }
    }

    // 返回当前内存池总共占用的字节数
    inline size_t size() const
    {
        size_t size = 0;
        PMEMORY_HEAD pMemNode = _Mem;
        while (pMemNode)
        {
            size += pMemNode->size;
            pMemNode = pMemNode->next;
        }
        return size;
    }

    // 初始化内存池, 内部会申请空间
    inline bool init(size_t size = 0x1000)
    {
        if (_Mem)
            return true;

        _Mem = malloc_head(size);
        _Now = _Mem;
        return true;
    }

    inline void swap(CMemoryObjectPool& other)
    {
        _Alloc&& _Al = std::move(other._Al);
        PMEMORY_HEAD _Mem = other._Mem;
        PMEMORY_HEAD _Now = other._Now;

        other._Mem = this->_Mem;
        other._Now = this->_Now;
        other._Al = std::move(this->_Al);

        this->_Mem = _Mem;
        this->_Now = _Now;
        this->_Al = std::move(_Al);
    }

    // 申请一个成员, 申请失败则抛出 std::bad_alloc 类型异常
    // 注意: 如果 sizeof(value_type) < sizeof(FREE_NODE), 用户数据会被槽位大小限制
    inline pointer malloc()
    {
        if (!_Mem)
            init();

        if (!_Mem)
            throw std::bad_alloc();

        // 第一步: 先看 _Now 能不能分配
        if (pointer p = try_alloc_from_block(_Now))
        {
            return p;  // _Now 不变, 下次继续从它分配
        }

        // 第二步: _Now 不够, 遍历所有块找还有空间的
        PMEMORY_HEAD pHead = _Mem;
        PMEMORY_HEAD pLastHead = pHead;
        while (pHead)
        {
            if (pHead == _Now)
            {
                pHead = pHead->next;
                continue;
            }

            if (pointer p = try_alloc_from_block(pHead))
            {
                // 先判断刚分配的这个块是否还有空间
                if (has_free_space(pHead))
                {
                    _Now = pHead;  // 还有空间, 切换到它
                    return p;
                }
                // 刚分配的块耗尽了, 往后找下一个有空间的块
                _Now = find_next_available_block(pHead, &pLastHead);
                if (!_Now)
                    _Now = expand(pLastHead);  // 所有块都没有可用内存, 申请新块
                return p;
            }
            pLastHead = pHead;
            pHead = pHead->next;
        }

        // 第三步: 所有块都没有可用内存, 申请新块
        _Now = expand(pLastHead);

        // 从新块分配
        byte_pointer ptr = _Now->item;
        _Now->item += SLOT_SIZE;
        return reinterpret_cast<pointer>(ptr);
    }

    // 释放一个成员, 链入对应内存块的回收链表
    inline bool free(pointer p)
    {
        PMEMORY_HEAD pHead = get_head(p);
        if (!pHead)
        {
            throw std::exception(__FUNCTION__ ": 传递进来了不是内存池里的地址", 1);
            return false;
        }

        auto pStart = reinterpret_cast<byte_pointer>(pHead);
        auto nodeOffset = static_cast<uint32_t>(reinterpret_cast<byte_pointer>(p) - pStart);

        // 节点 next 指向当前 head, head 指向新节点
        auto pNode = reinterpret_cast<FREE_NODE*>(reinterpret_cast<byte_pointer>(p));
        *pNode = pHead->head;  // 写入 next 偏移量
        pHead->head = nodeOffset;
        return true;
    }

    // 查询地址是否是内存池里的地址
    inline bool query(pointer p) const
    {
        return get_head(p) != nullptr;
    }

    // 输出当前内存池的状态
    inline void dump() const
    {
        PMEMORY_HEAD pHead = _Mem;
        int index = 0;
        while (pHead)
        {
            byte_pointer pAllocStart = reinterpret_cast<byte_pointer>(pHead) + sizeof(MEMORY_HEAD);
            int count = static_cast<int>(((pHead->item - pAllocStart) / SLOT_SIZE));
            printf("%03d: 内存块首地址 0x%p, 起始分配地址: 0x%p, 尺寸: %u, 已分配 %d 个成员, head偏移量 = %u\n",
                   index++, pHead, pAllocStart, (uint32_t)pHead->size, count, pHead->head);
            pHead = pHead->next;
        }
    }

private:
    // 检测内存块是否还有空闲内存可分配 (free list 或 item)
    inline bool has_free_space(PMEMORY_HEAD pHead) const
    {
        if (pHead->head != 0)
            return true;
        byte_pointer pEnd = reinterpret_cast<byte_pointer>(pHead) + pHead->size;
        return pHead->item + SLOT_SIZE <= pEnd;
    }

    // 从指定块开始遍历, 找到下一个还有空闲内存的块, 找不到返回 nullptr
    // 跳过 pStart 本身
    // 如果返回nullptr, 那么 pLastHead 就是接收最后一个内存块的地址
    inline PMEMORY_HEAD find_next_available_block(PMEMORY_HEAD pStart, PMEMORY_HEAD* pLastHead) const
    {
        *pLastHead = pStart;
        PMEMORY_HEAD pHead = pStart->next;
        while (pHead)
        {
            *pLastHead = pHead;
            if (has_free_space(pHead))
                return pHead;
            pHead = pHead->next;
        }
        return nullptr;
    }

    // 从指定内存块尝试分配, 返回 nullptr 表示该块无可用内存
    inline pointer try_alloc_from_block(PMEMORY_HEAD pHead)
    {
        // 从 item 分配
        byte_pointer pEnd = reinterpret_cast<byte_pointer>(pHead) + pHead->size;
        byte_pointer ptr = pHead->item;
        if (ptr + SLOT_SIZE <= pEnd)
        {
            pHead->item += SLOT_SIZE;
            return reinterpret_cast<pointer>(ptr);
        }

        // 从 free list 分配
        if (pHead->head != 0)
        {
            auto pStart = reinterpret_cast<byte_pointer>(pHead);
            auto pNode = reinterpret_cast<FREE_NODE*>(pStart + pHead->head);
            uint32_t nextOffset = *pNode;
            pHead->head = nextOffset;
            return reinterpret_cast<pointer>(pNode);
        }

        return nullptr;
    }

    // 检测地址是否在内存块的有效槽位范围内
    inline bool is_head(PMEMORY_HEAD pHead, const void* p) const
    {
        byte_pointer ptr = reinterpret_cast<byte_pointer>(const_cast<void*>(p));
        byte_pointer pStart = reinterpret_cast<byte_pointer>(pHead) + sizeof(MEMORY_HEAD);
        byte_pointer pEnd = pStart + pHead->size - sizeof(MEMORY_HEAD);

        return ptr >= pStart && ptr <= pEnd;
        if (ptr >= pStart && ptr <= pEnd)
        {
            const size_t offset = ptr - pStart;
            if (offset % SLOT_SIZE == 0)
                return true;
        }
        return false;
    }

    // 返回地址所属的内存块
    inline PMEMORY_HEAD get_head(const void* p) const
    {
        PMEMORY_HEAD pMemNode = _Mem;
        while (pMemNode)
        {
            if (is_head(pMemNode, p))
                return pMemNode;
            pMemNode = pMemNode->next;
        }
        return nullptr;
    }

    // 分配一块内存
    inline PMEMORY_HEAD malloc_head(size_t count)
    {
        auto newSize = sizeof(MEMORY_HEAD) + count * SLOT_SIZE;
        auto pStart = reinterpret_cast<byte_pointer>(_Al.allocate(newSize));

        PMEMORY_HEAD pHead = reinterpret_cast<PMEMORY_HEAD>(pStart);
        pHead->next = nullptr;
        pHead->size = newSize;
        pHead->item = pStart + sizeof(MEMORY_HEAD);
        pHead->head = 0;
        return pHead;
    }

    // 扩充内存: 从指定块开始找到链表尾部, 在尾部追加一个新块
    // count 为新块的槽位数量, 默认为给定块的 2 倍 + 1
    inline PMEMORY_HEAD expand(PMEMORY_HEAD pHead, size_t count = 0)
    {
        if (!pHead)
            return nullptr;

        // 找到链表尾部
        PMEMORY_HEAD pTail = pHead;
        while (pTail->next)
            pTail = pTail->next;

        // 计算新块大小
        if (count == 0)
            count = (pHead->size - sizeof(MEMORY_HEAD)) / SLOT_SIZE * 2 + 1;

        auto pNew = malloc_head(count);
        pTail->next = pNew;
        return pNew;
    }
};

NAMESPACE_MEMORYPOOL_END
