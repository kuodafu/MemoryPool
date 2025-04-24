#pragma once
#include "CMemoryObjectPool.h"
NAMESPACE_MEMORYPOOL_BEGIN


// 变长内存池, 目前底层是用定长内存池实现
// 最低分配字节数是 sizeof(PVOID)

class CMemoryPool
{
private:
#if CMEMORYPOOL_ISDEBUG
    using value_type = CMemoryObjectPool::value_type;
    CMemoryObjectPool m_Pool;
#else
    using value_type = PVOID;
    CMemoryObjectPool<value_type> m_Pool;
#endif
    using pointer = value_type*;


public:
    CMemoryPool(): m_Pool()
    {

    }

    CMemoryPool(const CMemoryPool& other) = delete;
    CMemoryPool& operator=(const CMemoryPool& other) = delete;
    CMemoryPool(CMemoryPool&& other) noexcept = delete;

    ~CMemoryPool()
    {
    }

public:
    // 初始化内存池, 预先分配多少个sizeof(PVOID)
    inline bool init(size_t size = 0x1000)
    {
        return m_Pool.init(size);
    }

    // 申请指定尺寸的内存, 申请失败则抛出 std::bad_alloc 类型异常
    inline PVOID malloc(size_t size, bool isClear = false)
    {
        const int cbSize = sizeof(PVOID);

        // size 字节尺寸需要的成员数, 加一个成员存放分配的成员数
        const int count = (((int)size + cbSize - 1) / cbSize) + 1;

        // 申请内存, 失败则抛出 std::bad_alloc 类型异常, 所以这里不需要处理失败的情况
        LPBYTE pStart = reinterpret_cast<LPBYTE>(m_Pool.malloc_arr(count, isClear));
        *(size_t*)pStart = count;

        // 第一个成员存放分配的成员数, 第二个成员开始是返回的内存
        LPBYTE pData = pStart + cbSize;
        return pData;
    }

    // 释放内存, 如果传递的不是内存池分配出去的地址, 会抛出 std::exception 异常
    inline bool free(PVOID p)
    {
        if (!p)
            return false;
        const int cbSize = sizeof(PVOID);

        // 指针前移 sizeof(PVOID) 字节, 指向分配的内存
        LPBYTE pData = reinterpret_cast<LPBYTE>(p);
        LPBYTE pStart = pData - cbSize;
        size_t count = *(size_t*)pStart;
        pointer pFree = reinterpret_cast<pointer>(pStart);

        // 释放内存
        return m_Pool.free_arr(pFree, count);
    }

    // 查询地址是否是内存池里的地址
    inline bool query(pointer p) const
    {
        if (!p)
            return false;
        const int cbSize = sizeof(PVOID);
        // 指针前移 sizeof(PVOID) 字节, 指向分配的内存
        LPBYTE pData = reinterpret_cast<LPBYTE>(p);
        pointer pStart = reinterpret_cast<pointer>(pData - cbSize);
        return m_Pool.query(pStart);
    }
};


NAMESPACE_MEMORYPOOL_END


