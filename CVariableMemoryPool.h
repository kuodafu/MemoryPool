#pragma once
#include <CMemoryPoolBase.h>
#include <CMemoryObjectPool.h>
#include <vector>

NAMESPACE_MEMORYPOOL_BEGIN

//------------------------------------------------------------
// 分桶式变长内存池
//
// 思想: 用多个定长池覆盖常用尺寸, 超出范围的走系统分配
//
// 尺寸分桶:
//   Bucket 0:  1 ~  8 字节  -> 槽位 8 字节
//   Bucket 1:  9 ~ 16 字节  -> 槽位 16 字节
//   Bucket 2: 17 ~ 32 字节  -> 槽位 32 字节
//   Bucket 3: 33 ~ 64 字节  -> 槽位 64 字节
//   Bucket 4: 65 ~ 128 字节 -> 槽位 128 字节
//   Bucket 5: 129 ~ 256 字节-> 槽位 256 字节
//   Bucket 6: > 256 字节    -> 直接系统分配 (不走池)
//
// 分配流程:
//   1. 根据 size 计算所属 bucket 索引
//   2. size > 256: 直接系统分配, 记录在溢出链表
//   3. size <= 256: 从对应定长池分配
//
// 释放流程:
//   1. 先在溢出链表中查找, 判断是池分配还是溢出分配
//   2. 池分配: 归还到对应桶的定长池
//   3. 溢出分配: 直接系统释放
//------------------------------------------------------------
class CVariableMemoryPool
{
public:
    using byte_pointer = uint8_t*;
    using const_byte_pointer = const uint8_t*;

private:
    //------------------------------------------------------------
    // 溢出块头: 跟踪每个 > 256 字节的独立分配
    //------------------------------------------------------------
    struct OverflowBlock
    {
        OverflowBlock*   next;
        size_t            userSize;   // 用户请求的字节数
        // 用户数据从这里开始
    };

    //------------------------------------------------------------
    // 池条目: 管理每个定长池
    //------------------------------------------------------------
    struct PoolEntry
    {
        size_t            slotSize;   // 槽位大小
        size_t            minSize;    // 本桶最小接受的字节数
        size_t            maxSize;    // 本桶最大接受的字节数
        CMemoryBytePool*  pool;      // 定长字节池, 延迟 new
        bool              ready;     // 是否已初始化
    };

    static constexpr int NUM_BUCKETS = 6;
    static constexpr size_t BUCKET_SIZES[NUM_BUCKETS] = { 8, 16, 32, 64, 128, 256 };

public:
    //------------------------------------------------------------
    // 构造
    // @param initialSlots 每个定长池的初始槽位数, 默认 1024
    //------------------------------------------------------------
    explicit CVariableMemoryPool(size_t initialSlots = 0x400)
        : _OverflowList(nullptr)
        , _OverflowCount(0)
        , _TotalOverflowBytes(0)
        , _InitialSlots(initialSlots)
    {
        for (int i = 0; i < NUM_BUCKETS; ++i)
        {
            _PoolEntries[i].slotSize = BUCKET_SIZES[i];
            _PoolEntries[i].minSize = (i == 0) ? 1 : (BUCKET_SIZES[i - 1] + 1);
            _PoolEntries[i].maxSize = BUCKET_SIZES[i];
            _PoolEntries[i].pool = nullptr;
            _PoolEntries[i].ready = false;
        }
    }

    ~CVariableMemoryPool()
    {
        // 清理所有桶
        for (int i = 0; i < NUM_BUCKETS; ++i)
        {
            if (_PoolEntries[i].pool)
            {
                delete _PoolEntries[i].pool;
                _PoolEntries[i].pool = nullptr;
            }
        }

        // 溢出块全部释放
        OverflowBlock* p = _OverflowList;
        while (p)
        {
            OverflowBlock* next = p->next;
            ::operator delete(p);
            p = next;
        }
        _OverflowList = nullptr;
    }

    //------------------------------------------------------------
    // 分配内存
    // @param size 要分配的字节数, 0 返回 nullptr
    // @return 分配的内存地址
    // @exception std::bad_alloc 分配失败
    //------------------------------------------------------------
    void* malloc(size_t size)
    {
        if (size == 0)
            return nullptr;

        int bucketIdx = _FindBucket(size);
        if (bucketIdx >= 0)
        {
            return _PoolAlloc(bucketIdx);
        }

        return _OverflowAlloc(size);
    }

    //------------------------------------------------------------
    // 释放内存
    // @param p 要释放的地址
    // @return true 成功; false 地址不属于本池
    //------------------------------------------------------------
    bool free(void* p)
    {
        if (!p)
            return true;

        // 先尝试溢出链表
        if (_IsOverflowBlock(p))
        {
            _OverflowFree(p);
            return true;
        }

        // 尝试从各个桶释放
        for (int i = 0; i < NUM_BUCKETS; ++i)
        {
            if (_PoolContains(i, p))
            {
                _PoolFree(i, p);
                return true;
            }
        }

        return false;
    }

    //------------------------------------------------------------
    // 查询地址是否属于本池
    //------------------------------------------------------------
    bool contains(void* p) const
    {
        if (!p)
            return false;
        if (_IsOverflowBlock(p))
            return true;
        for (int i = 0; i < NUM_BUCKETS; ++i)
        {
            if (_PoolContains(i, p))
                return true;
        }
        return false;
    }

    //------------------------------------------------------------
    // 查询当前已分配总字节数 (不含开销)
    // @note 溢出块字节数精确, 池内字节数为估算
    //------------------------------------------------------------
    size_t used_size() const
    {
        return _TotalOverflowBytes + _EstimatePoolUsed();
    }

    //------------------------------------------------------------
    // 查询向系统申请的总内存
    //------------------------------------------------------------
    size_t total_size() const
    {
        size_t total = _TotalOverflowBytes;
        for (int i = 0; i < NUM_BUCKETS; ++i)
        {
            if (_PoolEntries[i].pool)
                total += _PoolEntries[i].pool->size();
        }
        return total;
    }

    //------------------------------------------------------------
    // 查询溢出块数量
    //------------------------------------------------------------
    size_t overflow_count() const { return _OverflowCount; }

    //------------------------------------------------------------
    // 查询溢出块总字节数
    //------------------------------------------------------------
    size_t overflow_bytes() const { return _TotalOverflowBytes; }

    //------------------------------------------------------------
    // 清空: 重置所有池 (不释放内存)
    //------------------------------------------------------------
    void clear()
    {
        for (int i = 0; i < NUM_BUCKETS; ++i)
        {
            if (_PoolEntries[i].pool)
                _PoolEntries[i].pool->clear();
        }
    }

    //------------------------------------------------------------
    // 销毁: 释放所有内存
    //------------------------------------------------------------
    void destroy()
    {
        for (int i = 0; i < NUM_BUCKETS; ++i)
        {
            if (_PoolEntries[i].pool)
            {
                _PoolEntries[i].pool->destroy();
                delete _PoolEntries[i].pool;
                _PoolEntries[i].pool = nullptr;
                _PoolEntries[i].ready = false;
            }
        }

        OverflowBlock* p = _OverflowList;
        while (p)
        {
            OverflowBlock* next = p->next;
            ::operator delete(p);
            p = next;
        }
        _OverflowList = nullptr;
        _OverflowCount = 0;
        _TotalOverflowBytes = 0;
    }

    //------------------------------------------------------------
    // 调试: 打印内存池状态
    //------------------------------------------------------------
    void dump() const
    {
        printf("=== CVariableMemoryPool ===\n");
        printf("Overflow blocks: %zu, bytes: %zu\n", _OverflowCount, _TotalOverflowBytes);
        printf("\nBuckets:\n");
        for (int i = 0; i < NUM_BUCKETS; ++i)
        {
            const PoolEntry& e = _PoolEntries[i];
            size_t poolSize = e.pool ? e.pool->size() : 0;
            printf("  [%d] slot=%zu bytes, range=[%zu, %zu], poolSize=%zu, ready=%d\n",
                i, e.slotSize, e.minSize, e.maxSize, poolSize, e.ready);
        }
        printf("============================\n");
    }

private:
    //------------------------------------------------------------
    // 根据 size 找对应的桶索引, 找不到返回 -1
    //------------------------------------------------------------
    int _FindBucket(size_t size) const
    {
        if (size > BUCKET_SIZES[NUM_BUCKETS - 1])
            return -1;
        for (int i = 0; i < NUM_BUCKETS; ++i)
        {
            if (size <= BUCKET_SIZES[i])
                return i;
        }
        return -1;
    }

    //------------------------------------------------------------
    // 确保指定桶的池已初始化
    //------------------------------------------------------------
    void _EnsurePool(int bucketIdx)
    {
        PoolEntry& e = _PoolEntries[bucketIdx];
        if (!e.ready)
        {
            e.pool = new CMemoryBytePool(e.slotSize, _InitialSlots);
            e.ready = true;
        }
    }

    //------------------------------------------------------------
    // 从指定桶分配
    //------------------------------------------------------------
    void* _PoolAlloc(int bucketIdx)
    {
        _EnsurePool(bucketIdx);
        return _PoolEntries[bucketIdx].pool->malloc();
    }

    //------------------------------------------------------------
    // 释放到指定桶
    //------------------------------------------------------------
    void _PoolFree(int bucketIdx, void* p)
    {
        _PoolEntries[bucketIdx].pool->free(p);
    }

    //------------------------------------------------------------
    // 判断地址是否属于指定桶的池
    //------------------------------------------------------------
    bool _PoolContains(int bucketIdx, void* p) const
    {
        if (!_PoolEntries[bucketIdx].pool)
            return false;
        return _PoolEntries[bucketIdx].pool->query(p);
    }

    //------------------------------------------------------------
    // 估算池已使用的字节数
    // @note CMemoryBytePool 没有暴露已分配计数, 这里返回 0
    //       如需精确值, 可在分配/释放时累积计数
    //------------------------------------------------------------
    size_t _EstimatePoolUsed() const
    {
        return 0;
    }

    //------------------------------------------------------------
    // 溢出分配: 直接走系统分配
    //------------------------------------------------------------
    void* _OverflowAlloc(size_t userSize)
    {
        size_t totalSize = sizeof(OverflowBlock) + userSize;
        OverflowBlock* p = static_cast<OverflowBlock*>(::operator new(totalSize));
        if (!p)
            throw std::bad_alloc();

        p->userSize = userSize;
        p->next = _OverflowList;
        _OverflowList = p;
        _OverflowCount++;
        _TotalOverflowBytes += userSize;

        return p + 1;   // 用户数据在 header 之后
    }

    //------------------------------------------------------------
    // 溢出释放
    //------------------------------------------------------------
    void _OverflowFree(void* userPtr)
    {
        OverflowBlock* block = static_cast<OverflowBlock*>(userPtr) - 1;

        // 从链表中移除 (O(n), n 是溢出块数量)
        OverflowBlock** pp = &_OverflowList;
        while (*pp && *pp != block)
            pp = &((*pp)->next);

        if (*pp == block)
        {
            *pp = block->next;
            _OverflowCount--;
            _TotalOverflowBytes -= block->userSize;
            ::operator delete(block);
        }
    }

    //------------------------------------------------------------
    // 判断地址是否是溢出块
    //------------------------------------------------------------
    bool _IsOverflowBlock(void* p) const
    {
        OverflowBlock* block = _OverflowList;
        while (block)
        {
            byte_pointer blockStart = reinterpret_cast<byte_pointer>(block);
            byte_pointer userStart = blockStart + sizeof(OverflowBlock);
            byte_pointer userEnd = userStart + block->userSize;
            byte_pointer ptr = reinterpret_cast<byte_pointer>(p);

            if (ptr >= userStart && ptr < userEnd)
                return true;
            block = block->next;
        }
        return false;
    }

private:
    // 禁用拷贝
    CVariableMemoryPool(const CVariableMemoryPool&) = delete;
    CVariableMemoryPool& operator=(const CVariableMemoryPool&) = delete;

private:
    OverflowBlock*     _OverflowList;         // 溢出块链表 (每个 >256 字节分配)
    size_t              _OverflowCount;       // 溢出块数量
    size_t              _TotalOverflowBytes;  // 溢出块总字节数 (精确)
    PoolEntry           _PoolEntries[NUM_BUCKETS];
    size_t              _InitialSlots;        // 每个池的初始槽位数
};

NAMESPACE_MEMORYPOOL_END
