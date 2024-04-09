#pragma once
#include "CMemoryAllocator.h"
NAMESPACE_MEMORYPOOL_BEGIN

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


#if CMEMORYPOOL_ISDEBUG
class CMemoryPoolView;
#else
template<class _Ty = LPVOID> class CMemoryPoolView;
#endif

// �����ڴ��, ÿ�η��䶼�ǹ̶���С���ڴ�
#if CMEMORYPOOL_ISDEBUG
typedef int _Ty;
#else
template<class _Ty = LPVOID, class _Alloc = std::allocator<BYTE>>
#endif
class CMemoryObjectPool
{
#if CMEMORYPOOL_ISDEBUG
    using _Alloc = std::allocator<BYTE>;
#endif

private:


    using value_type    = _Ty;
    using pointer       = _Ty*;
    using const_pointer = const _Ty*;
    friend class CMemoryPoolView<value_type>;


    _Alloc              _Al;        // ������, �����ڴ涼�ǰ��ֽڷ���, �Լ���������Ա��Ҫ�����ֽ� + ͷ���ṹ
    PMEMORY_HEAD        _Mem;       // �ڴ��ͷ�ṹ, ����ͻ��ն���������ṹ������
    PMEMORY_HEAD        _Now;       // ��ǰ�������ڴ��

public:
    CMemoryObjectPool() :_Mem(0), _Al(), _Now(0)
    {

    }
    explicit CMemoryObjectPool(size_t size) :_Mem(0), _Al(), _Now(0)
    {
        init(size);
    }
    CMemoryObjectPool(const CMemoryObjectPool& other) = delete;
    CMemoryObjectPool& operator=(const CMemoryObjectPool& other) = delete;

    CMemoryObjectPool(CMemoryObjectPool&& other) noexcept :_Mem(0), _Al(), _Now(0)
    {
        _Al = std::move(other._Al);
        _Mem = other._Mem;
        _Now = other._Now;

        other._Mem = 0;
        other._Now = 0;
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
            _Al.deallocate(reinterpret_cast<LPBYTE>(node), node->size);
            node = next;
        }
        _Mem = 0;
        _Now = 0;
    }
    // ����Ѿ�������ڴ�, �����ͷ�, �Ѿ������ȥ���ڴ涼���ٿ���
    // ��պ��´δ�ͷ��ʼ�����ڴ�
    inline void clear()
    {
        if (!_Mem)
            return;

        // �������ڴ�鶼�ָ����������״̬

        PMEMORY_HEAD pHead = _Mem;
        _Now = _Mem;    // ��ǰ�������ڴ��ָ���һ���ڴ��
        while (pHead)
        {
            // arrָ���һ�������ȥ���ڴ�, ����Ϊ��ʼ״̬
            pHead->arr = reinterpret_cast<LPBYTE>(pHead) + sizeof(MEMORY_HEAD);
            pHead->pFree = 0;   // û�л��յ��ڴ�

            pHead = pHead->next;
        }
    }
public:
    // ��ʼ���ڴ��, �ڲ�������ռ�
    inline bool init(size_t size = 0x1000)
    {
        if (_Mem)
            return true;

        _Mem = malloc_head(size);
        _Now = _Mem;        // ��ǰ�������ڴ��
        return true;
    }

    // ����һ����Ա, ����ʧ�����׳� std::bad_alloc �����쳣
    inline pointer malloc(bool isClear = false)
    {
        return malloc_arr(1, isClear);
    }


    // ����һ������, �ͷ��ڴ���ʹ�� free_arr() �ͷ�, ����ֻ���ͷŵ�һ����Ա
    // ����ʧ�����׳� std::bad_alloc �����쳣
    // _Size = ������ٸ���Ա
    inline pointer malloc_arr(int _Size, bool isClear = false)
    {
        if (_Size < 0 || _Size > 0x7fffffff)
            _Size = 1;

        if (!_Mem)
            init();

        if (!_Mem)  // ��ʼ��ʧ��, Ӧ�ò���������, ��ʼ��ʧ�ܾ��Ƿ����ڴ�ʧ��, �Ѿ��׳��쳣��
            throw std::bad_alloc();
        

        PMEMORY_HEAD pHead = _Now;  // ������ڴ�鿪ʼ����
        pointer pRet = 0;

        auto pfn_ret = [isClear, _Size, &pRet, this](PMEMORY_HEAD pHead) -> bool
        {
            // �ȴ����������, ��������������, �ʹ����������
            if (pHead->arr)
                pRet = alloc_for_arr(pHead, _Size, isClear);
            
            // û��ֵ, �Ǿ���û�п��е��ڴ�, �ӻ��յ��ڴ���ȡ
            if (!pRet && pHead->pFree)
                pRet = alloc_for_listnode(pHead, _Size, isClear);
            


            return pRet != 0;
        };


        do
        {
            // ��ָ���ڴ��������ڴ�
            if (pfn_ret(pHead))
                return pRet;


            // �ߵ�������ǵ�ǰ�ڴ��û��ʣ���ڴ�ɷ���, ��Ҫ����һ���ڴ濪ʼ����
            // ����һ���ڴ濪ʼ����, ���û����һ���ڴ�, �������µ��ڴ�
            if (pHead->next)
            {
                // ������һ���ڴ�, ����һ���ڴ��������, ��ǰ�ڴ��ָ����һ���ڴ��
                pHead = pHead->next;
                _Now = pHead;
                continue;
            }

            // �ߵ�������ǵ�ǰ�ڴ��û����һ���ڴ���, ö��һ�������ڴ��, ����п��е��ڴ��, �ʹӿ��е��ڴ�鿪ʼ����
            if (pHead != _Mem)
            {
                PMEMORY_HEAD pMemNode = _Mem;
                while (pMemNode)
                {
                    // ������ǰ�ڴ��
                    if (_Now != pMemNode && pfn_ret(pMemNode))
                        return pRet;
                    pMemNode = pMemNode->next;
                }
                // ��������ѭ��û�з���, �Ǿ���û�п��õ��ڴ���, ��Ҫ�����µ��ڴ�
            }


            // �ߵ��������û����һ���ڴ�, Ҳû�п����ڴ����, ��Ҫ����һ���µ��ڴ�
            // pHead �Ѿ�ָ�����һ���ڴ����, 

            // TODO ����, ÿ���·�����ڴ�Ӧ�÷�����
            const size_t oldCount = (pHead->size - sizeof(MEMORY_HEAD)) / sizeof(value_type);
            const size_t scaleCount = oldCount * 2; // ÿ�η�����ڴ�����һ�ε�2��
            const size_t newCount = (_Size + scaleCount);   // һ��Ҫ��֤��η����ܴ�����������ڴ��С


            _Now = malloc_head(newCount);   // ��ǰ�������ڴ��ָ���·�����ڴ��
            pHead->next = _Now;             // �·�����ڴ�������������
            pHead = _Now;                   // pHeadָ���·�����ڴ��, ����ѭ�������ڴ�

        } while (true);

        // ���ߵ�����Ŀ϶�����û����ȷ�ķ����ڴ��ȥ, �׳��������ڴ��쳣
        throw std::bad_alloc();
        return 0;
    }

    // ���·�������, ���ԭ�������ݿ������µ��ڴ���
    // pArr = ��Ҫ���·���������ַ
    // _OldSize = ԭ���������Ա��
    // _Size = �������Ա��
    inline pointer realloc_arr(pointer pArr, size_t _OldSize, size_t _Size)
    {
        PMEMORY_HEAD pHead = GetHead(pArr);
        if (!pHead)
        {
            throw std::exception(__FUNCTION__ ": ���ݽ����˲����ڴ����ĵ�ַ", 0);
            return 0;
        }

        pointer pRet = 0;
        LPBYTE pEnd = reinterpret_cast<LPBYTE>(pHead) + pHead->size;
        LPBYTE pArrStart = reinterpret_cast<LPBYTE>(pArr);
        LPBYTE pArrEnd = pArrStart + sizeof(value_type) * _OldSize;
        bool isFree = true;
        if (pArrEnd == pHead->arr || pArrEnd == pEnd)
        {
            // ���ͷŵ����������������һ��������ڴ�, ֱ�Ӱ���һ��������ڴ�ָ����յ��������
            pHead->arr = pArrStart;
            pRet = alloc_for_arr(pHead, _Size, false);
            if (pRet)
                return pRet;    // �������, �Ǿ�ֻ����ָ��λ��, �������ڴ�
            isFree = false;     // pHead->arr = pArrStart; ��һ���Ѿ��ͷ���, �ѱ�������Ϊ��, ���治��Ҫ�����ͷ�
        }

        // �ߵ����������Ҫ���·���һ������, Ȼ�󿽱�ԭ�������ݵ��µ��ڴ���
        if (isFree)
        {
            // �ȳ��Դӵ�ǰ�ڴ�������, �������ʧ�ܾʹ������ڴ�������
            pRet = alloc_for_arr(pHead, _Size, false);
        }

        if (!pRet)
            pRet = malloc_arr(_Size, false);

        const size_t newSize = min(_OldSize, _Size);
        memcpy(pRet, pArr, sizeof(value_type) * newSize);

        if (isFree)
        {
            // Ȼ�����ԭ��������
            combine_free_pointer(pHead, pArrStart, pArrEnd, _OldSize);
        }
        return pRet;
    }

    // �ͷ�����, ���� malloc_arr ����ı����������ͷ�, ��Ȼ����Щ�ڴ��޷��ٴα�����
    inline bool free_arr(pointer p, size_t _Size)
    {
        PMEMORY_HEAD pHead = GetHead(p);
        if (!pHead)
        {
            throw std::exception(__FUNCTION__ ": ���ݽ����˲����ڴ����ĵ�ַ", 0);
            return false;
        }

        LPBYTE pArrStart = reinterpret_cast<LPBYTE>(p);
        LPBYTE pArrEnd = pArrStart + sizeof(value_type) * _Size;
        LPBYTE pEnd = reinterpret_cast<LPBYTE>(pHead) + pHead->size;
        if (pArrEnd == pHead->arr || pArrEnd == pEnd)
        {
            // ���ͷŵ����������������һ��������ڴ�, ֱ�Ӱ���һ��������ڴ�ָ����յ��������
            pHead->arr = pArrStart;
            return true;
        }

        combine_free_pointer(pHead, pArrStart, pArrEnd, _Size);

        //TODO �����Ƿ�������ó� ��ǰ�ڴ��ָ����ջ�������ڴ����ڵĿ�, �����´η����ʱ��Ϳ��Դ�����������
        _Now = pHead;
        return true;
    }

    inline bool free(pointer p)
    {
        return free_arr(p, 1);
    }

    // ��ѯ��ַ�Ƿ����ڴ����ĵ�ַ
    inline bool query(_Ty* p) const
    {
        return GetHead(p) != nullptr;
    }
    // �����ǰ�ڴ�ص�״̬, ���ÿ���ڴ���״̬
    inline void dump() const
    {
        PMEMORY_HEAD pHead = _Mem;
        int index = 0;
        while (pHead)
        {
            LPBYTE pAllocStart = reinterpret_cast<LPBYTE>(pHead) + sizeof(MEMORY_HEAD);
            int count = static_cast<int>(((pHead->arr - pAllocStart) / sizeof(value_type)));
            printf("%03d: �ڴ���׵�ַ 0x%p, �ڴ����ʼ�����ַ: 0x%p, �ڴ��ߴ�: %u, �ѷ��� %d ����Ա, ��ǰ���յ��ڴ����� = 0x%p\n",
                index++, pHead, pAllocStart, (UINT)pHead->size, count, pHead->pFree);
            pHead = pHead->next;
        }
    }

private:
    // ��������ַ�Ƿ�������ڴ��ĵ�ַ
    inline bool IsHead(PMEMORY_HEAD pHead, LPCVOID p) const
    {
        LPBYTE ptr = reinterpret_cast<LPBYTE>(const_cast<LPVOID>(p));

        // �׵�ַ������Ҫ�����ȥ�ĵ�ַ, ������ַ����һ������ĵ�ַ
        // �����һ������ĵ�ַΪ��, �Ǿ���ָ������ڴ�Ľ�����ַ
        LPBYTE pStart = reinterpret_cast<LPBYTE>(pHead) + sizeof(MEMORY_HEAD);
        //LPBYTE pEnd = pHead->arr ? pHead->arr : (pStart + pHead->size - sizeof(MEMORY_HEAD));
        LPBYTE pEnd = (pStart + pHead->size - sizeof(MEMORY_HEAD));
        if (ptr >= pStart && ptr <= pEnd)
        {
            // ����ĵ�ַ�����ǰ���Ա�ߴ����, ����������ǾͲ��ǵ�ʱ����ĵ�ַ
            const size_t offset = ptr - pStart;
            if (offset % sizeof(value_type) == 0)
                return true;    // ��ַ���ڵ����׵�ַ, С�ڽ�����ַ, ��������ڴ����ĵ�ַ
        }
        return false;
    }

    // ���������ַ��Ӧ���ڴ��ṹ, ��������ַ�����ڴ�صĵ�ַ, ����0
    inline PMEMORY_HEAD GetHead(LPCVOID p) const
    {
        PMEMORY_HEAD pMemNode = _Mem;
        while (pMemNode)
        {
            PMEMORY_HEAD next = pMemNode->next;
            if (IsHead(pMemNode, p))
                return pMemNode;
            pMemNode = next;
        }
        return 0;
    }
    // ���ڴ�������������Ա��ȥ
    inline pointer alloc_for_arr(PMEMORY_HEAD pHead, int _Size, bool isClear) const
    {
        LPBYTE pEnd = reinterpret_cast<LPBYTE>(pHead) + pHead->size;
        LPBYTE ptr = pHead->arr;
        const size_t offset = _Size * sizeof(value_type);
        if (ptr + offset > pEnd)
            return 0;   // �ڴ治������һ������, ����0

        pHead->arr += offset;    // ָ����һ������ĵ�ַ
        if (pHead->arr >= pEnd)
        {
            pHead->arr = 0;  // �Ѿ����䵽β����, ����Ϊ0
        }

        if (isClear)
            memset(ptr, 0, offset);
        return reinterpret_cast<pointer>(ptr);
    }

    // ��ȡָ���ڵ�ռ�ü�����Ա, �ڵ�Ϊ���򷵻�0, ��Ϊ�����ٷ���1, ���ش���1��ʾ�Ƕ����Ա
    int GetNodeCount(PMEMORY_HEAD pHead, PLIST_NODE node, PLIST_NODE& pNextNode) const
    {
        pNextNode = 0;
        if (!node)
            return 0;

        int count = 1;  // �ߵ������ʾ�����������нڵ�, �Ƿ��صĳ�Ա������Ϊ1

        // ���������׽ڵ����һ���ڵ㲻Ϊ0, �Ǿͱ�ʾnextҪô���ų�Ա��, Ҫô������һ���ڵ��ַ
        if (node->next != 0)
        {
            // next��Ϊ0, �Ǿ���Ҫô���Ա��, Ҫô����һ���ڵ�ָ��
            // pArr[0] == pHead->pFree->next, ��������һ����
            LPINT* pArr = reinterpret_cast<LPINT*>(node);
            if (IsHead(pHead, node->next))
            {
                pNextNode = node->next; // next��ŵ�����һ���ڵ��ַ
            }
            else
            {
                // next��ŵ��ǳ�Ա��
                count = reinterpret_cast<int>(pArr[0]);
                pNextNode = reinterpret_cast<PLIST_NODE>(pArr[1]);
            }
        }
        return count;
    }

    // ��ȡ���������׸��ڵ������, ����0��ʾû����һ���ڵ�
    // �������1��ʾ�׸��ڵ��ŵ�����һ���ڵ�ĵ�ַ
    // ���ش���1��ʾ�׽ڵ��ŵ���һ������, ���س�Ա��
    inline int get_first_node_count(PMEMORY_HEAD pHead, PLIST_NODE& pNextNode) const
    {
        pNextNode = 0;
        if (!pHead->pFree)
            return 0;
        return GetNodeCount(pHead, pHead->pFree, pNextNode);
    }
    // ���ڴ�������ڵ�������Ա��ȥ, ���ߵ���������Ļ�, ���յ�����϶���Ϊ��
    inline pointer alloc_for_listnode(PMEMORY_HEAD pHead, int _Size, bool isClear)
    {
        LPBYTE pStart = reinterpret_cast<LPBYTE>(pHead) + sizeof(MEMORY_HEAD);
        LPBYTE pEnd = pStart + pHead->size - sizeof(MEMORY_HEAD);

        // �׸��ڵ��Ƿ����ڴ����ĵ�ַ, �������, �Ǿͱ�ʾ������ڵ��ж��ٸ���Ա��������
        pointer* pArr = reinterpret_cast<pointer*>(pHead->pFree);
        if (!pArr)
            return 0;

        PLIST_NODE pNextNode = 0;
        int count = get_first_node_count(pHead, pNextNode);

        if (_Size == 1 && count == 1)
        {
            // ֻ����һ����Ա, �����׸��ڵ㲻�ǳ�Ա��, �Ǿ�ֱ�Ӱ�����ڵ�����ȥ, Ȼ��ָ����һ���ڵ�
            PLIST_NODE pNode = pHead->pFree; // ��ͷ�ڵ�����ȥ
            pHead->pFree = pNode->next;      // ͷ�ڵ�ָ����һ���ڵ�
            if (isClear)
                memset(pNode, 0, sizeof(value_type));
            return reinterpret_cast<pointer>(pNode);
        }

        if (count == 1)
        {
            //TODO �ߵ��������Ҫ��������Ա, ���ǻ��յ������׸���Ա���ǳ�Ա��, �����Ƿ��б�Ҫ�����������
            return 0;
        }

        // �ߵ���������׸��ڵ��ǳ�Ա��, ���ұ���Ҫ��������Ա, ����Ƿ��㹻����

        if (_Size > count)
            return 0;   // ���յ������Ա����������, ����0, ���ǿ����, �Ǳ��������ҷ��仹�ǲ���

        // �ߵ�������ǻ��յ������Ա���㹻����, �����ȥ, Ȼ���޸ĳ�Ա��



        if (_Size == count)
        {
            // �ڵ�ĳ�Ա�����ڷ����ȥ�ĳ�Ա��, �Ǿ�ֱ�ӷ����ȥ, Ȼ�������׽ڵ�ָ����һ���ڵ�
            pHead->pFree = pNextNode;
            if (isClear)
                memset(pArr, 0, sizeof(value_type) * _Size);
            return reinterpret_cast<pointer>(pArr);
        }

        // �ߵ�������Ƿ���ĳ�Ա��С�������Ա��, ��Ҫ�������Ա����ȥ����ĳ�Ա��, Ȼ��ѷ���ĳ�Ա������
        LPBYTE pNodeStart = reinterpret_cast<LPBYTE>(pHead->pFree);
        LPBYTE pNodeEnd = pNodeStart + (sizeof(value_type) * _Size);

        // ����������׽ڵ�ָ������ȥ�����һ����Ա��ַ
        pHead->pFree = reinterpret_cast<PLIST_NODE>(pNodeEnd);

        if (_Size + 1 == count)
        {
            // �����ȥ��ڵ��ʣ��һ����Ա, �Ǿ������Աָ����һ���ڵ�
            pHead->pFree->next = pNextNode; // ֻʣһ����Ա��, �Ǿ�ָ����һ���ڵ�
        }
        else
        {
            // �ߵ������ʣ��1����Ա����, �Ǿ͵�һ����Ա��¼ʣ���Ա��, �ڶ�����Աָ����һ���ڵ�

            pArr = reinterpret_cast<pointer*>(pNodeEnd);
            pArr[0] = reinterpret_cast<pointer>(count - _Size); // ��һ����Ա��¼ʣ���Ա��
            pArr[1] = reinterpret_cast<pointer>(pNextNode);     // �ڶ�����Աָ����һ���ڵ�
        }

        if (isClear)
            memset(pNodeStart, 0, sizeof(value_type) * _Size);
        return reinterpret_cast<pointer>(pNodeStart);
    }

    // ����һ���ڴ�, ��������Ҫ������ٸ���Ա
    inline PMEMORY_HEAD malloc_head(size_t count)
    {
        // TODO ����, ÿ���·�����ڴ�Ӧ�÷�����
        const size_t newSize = sizeof(MEMORY_HEAD) + count * sizeof(value_type);
        LPBYTE pStart = _Al.allocate(newSize);
        LPBYTE pEnd = pStart + newSize;

        PMEMORY_HEAD pHead = reinterpret_cast<PMEMORY_HEAD>(pStart);

        pHead->next     = 0;
        pHead->size     = newSize;
        pHead->arr      = pStart + sizeof(MEMORY_HEAD);
        pHead->pFree    = 0;
        return pHead;
    }



    // ���ͷŵĵ�ַ�ϲ���������, ������������ڴ�, �ͺϲ���һ���ڴ��, �����������, �ʹ�������
    // pHead = �ڴ��ͷ�ṹ
    // pFreeStart = �ͷŵ��ڴ濪ʼ��ַ
    // pFreeEnd = �ͷŵ��ڴ������ַ
    // _FreeCount = �ͷŵ��ڴ��Ա��
    inline void combine_free_pointer(PMEMORY_HEAD pHead, LPBYTE pFreeStart, LPBYTE pFreeEnd, int _FreeCount)
    {
        // �������������м��з����ȥ������, ���յ�������
        // ���յ�ʱ�򿴿���һ���ڵ�͵�ǰ���յ��ڴ��ǲ���������, ���������ڵ�/�ڵ���������, �����͵������鱣��
        // ���ڻ��յ�������������ģʽ
        // 1. ��һ�������ڴ����ĵ�ַ, �Ǿ��ǳ�Ա��, ��ʾ�����Ա��������ĳ�Ա�ж��ٸ���������, ���һ����Աָ����һ���ڵ�
        // 2. ��һ����Ա���ڴ����ĵ�ַ, �Ǿ���ָ����һ���ڵ�

        // �ڴ�����β��ַ, �жϻ��յ��ڴ��Ƿ���ڴ�����β��ַһ��, һ���ͱ�ʾ�����ڴ涼��������, ��ԭ����ʼ״̬
        LPBYTE pItemStart = reinterpret_cast<LPBYTE>(pHead) + sizeof(MEMORY_HEAD);
        LPBYTE pItemEnd = pItemStart + pHead->size - sizeof(MEMORY_HEAD);
        if (pFreeStart == pItemStart && (pHead->arr == 0 ? pFreeEnd == pItemEnd : pFreeEnd == pHead->arr))
        {
            // ���յ��������ڴ�, ��ԭ����ʼ״̬
            pHead->arr = pItemStart;
            pHead->pFree = 0;
            return;
        }

        // �׸��ڵ��Ƿ����ڴ����ĵ�ַ, �������, �Ǿͱ�ʾ������ڵ��ж��ٸ���Ա��������
        pointer* pArr = reinterpret_cast<pointer*>(pHead->pFree);
        PLIST_NODE pNextNode = 0;
        int count = get_first_node_count(pHead, pNextNode);
        
        // �ͷ������нڵ�����β��ַ, ��Ҫ�жϻ��յ��ڴ��ǲ��Ǻ������ַ��������
        LPBYTE pNodeStart = reinterpret_cast<LPBYTE>(pHead->pFree);
        LPBYTE pNodeEnd = pNodeStart + (sizeof(value_type) * count);

        // ��� �ͷŵĽ�����ַ����������Ŀ�ʼ��ַ, ���� �ͷŵĿ�ʼ��ַ����������Ľ�����ַ
        // �Ǿ��ǲ��������ڴ�, ֱ�Ӱѻ��յ��ڴ�ŵ�������
        if (pFreeEnd != pNodeStart && pFreeStart != pNodeEnd)
        {
            // �ߵ�������ǻ��յ��ڴ�����������, �ǾͰѻ��յ��ڴ�ŵ�������
            PLIST_NODE pFirstNode = reinterpret_cast<PLIST_NODE>(pFreeStart);
            if (_FreeCount == 1)
            {
                pFirstNode->next = pHead->pFree;    // ֻ�ͷ�һ����Ա, �Ǿͷŵ������׽ڵ�
                pHead->pFree = pFirstNode;          // �׽ڵ�ָ�򱾴λ��յ��ڴ�
                return;
            }
            // ������, ���Ǳ��λ��յĲ�ֹһ����Ա
            pArr = reinterpret_cast<pointer*>(pFirstNode);
            pArr[0] = reinterpret_cast<pointer>(_FreeCount);        // ��һ����Աָ���Ա��
            pArr[1] = reinterpret_cast<pointer>(pHead->pFree);      // �ڶ�����Աָ����һ���ڵ�
            pHead->pFree = pFirstNode;          // �׽ڵ�ָ�򱾴λ��յ��ڴ�
            return;
        }

        PLIST_NODE pFirstNode = pHead->pFree;   // �׽ڵ�
        if (pFreeEnd == pNodeStart)
        {
            // ���յĵ�ַ�����׽ڵ�, ���׽ڵ�͸ĳɻ��յĵ�ַ
            pFirstNode = reinterpret_cast<PLIST_NODE>(pFreeStart);
        }
        else
        {
            // �����׽ڵ����Ż��յ��ڴ�, �׽ڵ㻹��������׽ڵ�, ��Ҫ�޸ĳ�Ա��

        }


        // �ߵ�������ǻ��յ��ڴ�����������������
        // �������������Ѿ���������

        const int newCount = _FreeCount + count;

        pArr = reinterpret_cast<pointer*>(pFirstNode);
        pArr[0] = reinterpret_cast<pointer>(newCount);      // ��һ����Աָ���Ա��
        pArr[1] = reinterpret_cast<pointer>(pNextNode);     // �ڶ�����Աָ����һ���ڵ�
        pHead->pFree = pFirstNode;          // �׽ڵ�ָ�򱾴λ��յ��ڴ�

        // �ϲ������ڵ�ĵ�ַ
        combine_pointer(pHead, pNextNode);

    }

    // �ϲ��׽ڵ�ʹ���ڵ�ĵ�ַ, ��������ڵ�ĵ�ַ��������, �ǾͰ���һ���ڴ�ϲ���һ��
    void combine_pointer(PMEMORY_HEAD pHead, PLIST_NODE pNode2)
    {
        if (!pNode2)
            return; // �ڶ����ڵ��ǿսڵ�, ����Ҫ�ϲ�

        PLIST_NODE pNext1 = 0, pNext2 = 0, pFirstNode = 0, pNextNode = 0;
        int count1 = GetNodeCount(pHead, pHead->pFree, pNext1);
        int count2 = GetNodeCount(pHead, pNode2, pNext2);

        LPBYTE pNodeStart1 = reinterpret_cast<LPBYTE>(pHead->pFree);
        LPBYTE pNodeEnd1 = pNodeStart1 + (sizeof(value_type) * count1);

        LPBYTE pNodeStart2 = reinterpret_cast<LPBYTE>(pNode2);
        LPBYTE pNodeEnd2 = pNodeStart2 + (sizeof(value_type) * count2);

        if(pNodeStart1 == pNodeEnd2)
        {
            // �ڵ�2�Ľ�����ַ�ǽڵ�1�Ŀ�ʼ��ַ, �Ǿ��� �ڵ�2���Žڵ�1
            // �׽ڵ�ָ��ڵ�2, ��һ���ڵ���ǽڵ�2����һ���ڵ�
            // ��Ϊ��һ���ڵ�ָ��ľ��ǵڶ����ڵ�, ������һ���ڵ���Զ�ĵڶ����ڵ�ָ�����һ���ڵ�
            pFirstNode = pNode2;
            pNextNode = pNext2;
        }
        else if (pNodeEnd1 == pNodeStart2)
        {
            // �ڵ�1�Ľ�����ַ�ǽڵ�2�Ŀ�ʼ��ַ, �Ǿ��� �ڵ�1���Žڵ�2
            // �׽ڵ㻹�ǽڵ�1
            pFirstNode = pHead->pFree;
            pNextNode = pNext2;
        }
        else
        {
            // �����ڵ�ĵ�ַ��������, ����Ҫ�ϲ�
            return;
        }
        const int newCount = count1 + count2;

        pointer* pArr = reinterpret_cast<pointer*>(pFirstNode);
        pArr[0] = reinterpret_cast<pointer>(newCount);      // ��һ����Աָ���Ա��
        pArr[1] = reinterpret_cast<pointer>(pNextNode);     // �ڶ�����Աָ����һ���ڵ�
        pHead->pFree = pFirstNode;          // �׽ڵ�ָ�򱾴λ��յ��ڴ�

        // �ݹ�ϲ������ڵ�ĵ�ַ
        combine_pointer(pHead, pNextNode);

    }

};

#if CMEMORYPOOL_ISDEBUG
typedef int _Ty;
#else
template<class _Ty>
#endif
class CMemoryPoolView
{
    using value_type = _Ty;
    using MEMPOOL = CMemoryObjectPool<value_type>;

    MEMPOOL* pool;
public:

    CMemoryPoolView(MEMPOOL* pool = 0) : pool(pool)
    {

    }
    void init(MEMPOOL* pool)
    {
        this->pool = pool;
    }
    // ��ȡ�ڴ���׸��ڴ��
    PMEMORY_HEAD GetHead() const
    {
        return pool->_Mem;
    }
    // ��ȡ�ڴ�ص�ǰ�������ڴ��
    PMEMORY_HEAD GetNow() const
    {
        return pool->_Now;
    }
    // ��ȡָ���ڵ�ռ�ü�����Ա, �ڵ�Ϊ���򷵻�0, ��Ϊ�����ٷ���1, ���ش���1��ʾ�Ƕ����Ա
    int GetNodeCount(PMEMORY_HEAD pHead, PLIST_NODE node, PLIST_NODE& pNextNode)
    {
        return pool->GetNodeCount(pHead, node, pNextNode);
    }
    // ��ȡ��ʼ�����ڴ����ʼ��ַ�ͽ�����ַ, �����ж��ٸ���Ա
    int GetItemStartEnd(PMEMORY_HEAD pHead, LPBYTE& pStart, LPBYTE& pEnd)
    {
        pStart = (LPBYTE)pHead;
        pEnd = pStart + pHead->size;
        pStart = pStart + sizeof(MEMORY_HEAD);
        return (pEnd - pStart) / sizeof(value_type);
    }
    // ����һ����ַ, ȷ�������ַ���ڴ����ڼ�����Ա, ����ָ����Ա�ʹ���0xcc�ϵ�
    int PointerToIndex(PMEMORY_HEAD pHead, LPCVOID ptr)
    {
        LPBYTE pStart = (LPBYTE)pHead;
        LPBYTE pEnd = pStart + pHead->size;
        LPBYTE pFirst = pStart + sizeof(MEMORY_HEAD);

        const int index = IsItemAddress(ptr, pFirst, pEnd);
        if (index == -1)
            __debugbreak(); // �����˴���ĵ�ַ, ���µ���
        return index;
    }

    // �жϴ���ĵ�ַ�Ƿ��ǿ�ʼ�ͽ����ﲢ���Ƕ����Ա�ĵ�ַ, ���س�Ա����, ���ǳ�Ա��ַ�ͷ���-1
    int IsItemAddress(LPCVOID ptr, LPBYTE begin, LPBYTE end)
    {
        int index = 0;
        while (begin < end)
        {
            if (begin == (LPBYTE)ptr)
                return index;
            index++;
            begin += sizeof(value_type);
        }
        return -1;
    };

    // ��⴫��ĵ�ַ�Ƿ��ǻ���������ĵ�ַ
    bool IsFreeList(PMEMORY_HEAD pHead, LPCVOID ptr)
    {
        PLIST_NODE node = pHead->pFree;
        while (node)
        {
            PLIST_NODE pNextNode = 0;

            const int count = GetNodeCount(pHead, node, pNextNode);
            LPBYTE pNodeStart = reinterpret_cast<LPBYTE>(node);
            LPBYTE pNodeEnd = pNodeStart + (sizeof(value_type) * count);
            int index = IsItemAddress(ptr, pNodeStart, pNodeEnd);

            if (index != -1)
                return true;   // �ڻ��յ�������

            node = pNextNode;
        }
        return false;    // �����˻�������, û����������
    }

    // ����һ����ַ, �ж������ַ�Ƿ����ڴ����ĵ�ַ, ���ұ������ȥ, �������Ĳ����ڴ�صĵ�ַ�ʹ���0xcc�ϵ�
    int IsAllocatord(PMEMORY_HEAD pHead, LPCVOID ptr)
    {
        // ����һ���ж������ַ�Ƿ�Ϸ�, ������Ϸ�, ������������
        // ����౾��Ͳ���Ϊ��Ч��, ����Ϊ�˾��Ե�׼ȷ, �����������һ���жϵ�ַ�Ƿ�Ϸ�
        PointerToIndex(pHead, ptr);

        // �׵�ַ������Ҫ�����ȥ�ĵ�ַ, ������ַ����һ������ĵ�ַ
        // �����һ������ĵ�ַΪ��, �Ǿ���ָ������ڴ�Ľ�����ַ
        LPBYTE pStart = reinterpret_cast<LPBYTE>(pHead) + sizeof(MEMORY_HEAD);
        LPBYTE pEnd = pHead->arr ? pHead->arr : (pStart + pHead->size - sizeof(MEMORY_HEAD));
        LPBYTE pFirst = pStart;

        LPBYTE p = (LPBYTE)ptr;
        const int index = IsItemAddress(ptr, pFirst, pEnd);
        if (index == -1 && p >= pFirst && p < pEnd)
        {
            __debugbreak(); // �����˴���ĵ�ַ, ���µ���
            return false;
        }

        // û������ʼ���䵽��ǰ����ĵ�ַ��, �Ǿ���û�б������ȥ
        if (index == -1)
            return false;
        
        // �Ѿ�������������ȥ��, �������Ƿ��ڻ��յ�������, �����, �Ǿ���û�����ȥ
        return IsFreeList(pHead, ptr) == false;
    }

};

NAMESPACE_MEMORYPOOL_END


