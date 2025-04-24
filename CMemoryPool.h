#pragma once
#include "CMemoryObjectPool.h"
NAMESPACE_MEMORYPOOL_BEGIN


// �䳤�ڴ��, Ŀǰ�ײ����ö����ڴ��ʵ��
// ��ͷ����ֽ����� sizeof(PVOID)

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
    // ��ʼ���ڴ��, Ԥ�ȷ�����ٸ�sizeof(PVOID)
    inline bool init(size_t size = 0x1000)
    {
        return m_Pool.init(size);
    }

    // ����ָ���ߴ���ڴ�, ����ʧ�����׳� std::bad_alloc �����쳣
    inline PVOID malloc(size_t size, bool isClear = false)
    {
        const int cbSize = sizeof(PVOID);

        // size �ֽڳߴ���Ҫ�ĳ�Ա��, ��һ����Ա��ŷ���ĳ�Ա��
        const int count = (((int)size + cbSize - 1) / cbSize) + 1;

        // �����ڴ�, ʧ�����׳� std::bad_alloc �����쳣, �������ﲻ��Ҫ����ʧ�ܵ����
        LPBYTE pStart = reinterpret_cast<LPBYTE>(m_Pool.malloc_arr(count, isClear));
        *(size_t*)pStart = count;

        // ��һ����Ա��ŷ���ĳ�Ա��, �ڶ�����Ա��ʼ�Ƿ��ص��ڴ�
        LPBYTE pData = pStart + cbSize;
        return pData;
    }

    // �ͷ��ڴ�, ������ݵĲ����ڴ�ط����ȥ�ĵ�ַ, ���׳� std::exception �쳣
    inline bool free(PVOID p)
    {
        if (!p)
            return false;
        const int cbSize = sizeof(PVOID);

        // ָ��ǰ�� sizeof(PVOID) �ֽ�, ָ�������ڴ�
        LPBYTE pData = reinterpret_cast<LPBYTE>(p);
        LPBYTE pStart = pData - cbSize;
        size_t count = *(size_t*)pStart;
        pointer pFree = reinterpret_cast<pointer>(pStart);

        // �ͷ��ڴ�
        return m_Pool.free_arr(pFree, count);
    }

    // ��ѯ��ַ�Ƿ����ڴ����ĵ�ַ
    inline bool query(pointer p) const
    {
        if (!p)
            return false;
        const int cbSize = sizeof(PVOID);
        // ָ��ǰ�� sizeof(PVOID) �ֽ�, ָ�������ڴ�
        LPBYTE pData = reinterpret_cast<LPBYTE>(p);
        pointer pStart = reinterpret_cast<pointer>(pData - cbSize);
        return m_Pool.query(pStart);
    }
};


NAMESPACE_MEMORYPOOL_END


