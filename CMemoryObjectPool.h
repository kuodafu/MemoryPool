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
    PLIST_NODE      list;       // ���ջ������ڴ�, �����û�л���, �����ֵ����0, ����Ҳ�������˾���Ҫ�����µ��ڴ��
    LPBYTE          item;       // �����ȥ���ڴ�, ÿ�η����ȥ��ָ����һ����Ա, ֱ��Խ���ʹ�������ȡ��һ���ڵ�
}*PMEMORY_HEAD;


#if CMEMORYPOOL_ISDEBUG
class CMemoryPoolView;
#else
template<class _Ty = LPVOID, class _Alloc = std::allocator<BYTE>> class CMemoryPoolView;
#endif

// �����ڴ��, ÿ�η��䶼�ǹ̶���С���ڴ�
#if CMEMORYPOOL_ISDEBUG
typedef size_t _Ty;
#else
template<class _Ty = LPVOID, class _Alloc = std::allocator<BYTE>>
#endif
class CMemoryObjectPool
{
public:
    using value_type    = _Ty;
    using pointer       = _Ty*;
    using const_pointer = const _Ty*;
private:
#if CMEMORYPOOL_ISDEBUG
    using _Alloc = std::allocator<BYTE>;
    friend class CMemoryPoolView;
#else
    friend class CMemoryPoolView<value_type, _Alloc>;
#endif


    _Alloc              _Al;        // ������, �����ڴ涼�ǰ��ֽڷ���, �Լ���������Ա��Ҫ�����ֽ� + ͷ���ṹ
    PMEMORY_HEAD        _Mem;       // �ڴ��ͷ�ṹ, ����ͻ��ն���������ṹ������
    PMEMORY_HEAD        _Now;       // ��ǰ�������ڴ��

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
            _Al.deallocate(reinterpret_cast<LPBYTE>(node), node->size);
            node = next;
        }
        _Mem = nullptr;
        _Now = nullptr;
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
            pHead->item = reinterpret_cast<LPBYTE>(pHead) + sizeof(MEMORY_HEAD);
            pHead->list = nullptr;   // û�л��յ��ڴ�

            pHead = pHead->next;
        }
    }

    // ���ص�ǰ�ڴ���ܹ�ռ�õ��ֽ���, �����Ѿ������û�����
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
    // ����һ����Ա, ����ʧ�����׳� std::bad_alloc �����쳣
    inline pointer malloc(bool isClear = false)
    {
        return malloc_arr(1, isClear);
    }


    // ����һ������, �ͷ��ڴ���ʹ�� free_arr() �ͷ�, ����ֻ���ͷŵ�һ����Ա
    // ����ʧ�����׳� std::bad_alloc �����쳣
    // _Size = ������ٸ���Ա
    inline pointer malloc_arr(size_t _Size, bool isClear = false)
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
            // �ȴӻ������������, �������ʧ�ܾʹ������ڴ�������
            // ����������, ��Ҫ���������̫��, ����ᵼ���������ʱ�����
            if (pHead->list)
                pRet = alloc_for_list(pHead, _Size, isClear, 0);

            // �ٴ�item�����
            if (!pRet && pHead->item)
                pRet = alloc_for_item(pHead, _Size, isClear);
            
            return pRet != nullptr;
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
        PMEMORY_HEAD pHead = get_head(pArr);
        if (!pHead)
        {
            throw std::exception(__FUNCTION__ ": ���ݽ����˲����ڴ����ĵ�ַ", 1);
            return 0;
        }

        pointer pRet = 0;
        LPBYTE pEnd = reinterpret_cast<LPBYTE>(pHead) + pHead->size;
        LPBYTE pArrStart = reinterpret_cast<LPBYTE>(pArr);
        LPBYTE pArrEnd = pArrStart + sizeof(value_type) * _OldSize;
        bool isFree = true;
        if (pArrEnd == pHead->item || pArrEnd == pEnd)
        {
            // ���ͷŵ����������������һ��������ڴ�, ֱ�Ӱ���һ��������ڴ�ָ����յ��������
            pHead->item = pArrStart;
            isFree = false;     // pHead->arr = pArrStart; ��һ���Ѿ��ͷ���, �ѱ�������Ϊ��, ���治��Ҫ�����ͷ�

        }

        // �ȳ��Դӻ������������, �������ʧ�ܾʹ������ڴ�������
        // ����������, ��Ҫ���������̫��, ����ᵼ���������ʱ�����
        pRet = alloc_for_list(pHead, _Size, false, 0);
        if(pRet && !isFree)
            return pRet;    // �����һ���ڴ�����ɹ�, ����isFreeΪfalse, �Ǿ�����ԭ������ĵ�ַ�Ϸ���ɹ���
        
        if (!pRet && pHead->list)
            pRet = alloc_for_item(pHead, _Size, false);    // ���������ʧ�ܺ�ͳ��Դ�item�����

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
        PMEMORY_HEAD pHead = get_head(p);
        if (!pHead)
        {
            throw std::exception(__FUNCTION__ ": ���ݽ����˲����ڴ����ĵ�ַ", 1);
            return false;
        }

        LPBYTE pArrStart = reinterpret_cast<LPBYTE>(p);
        LPBYTE pArrEnd = pArrStart + sizeof(value_type) * _Size;
        LPBYTE pEnd = reinterpret_cast<LPBYTE>(pHead) + pHead->size;
        if (pArrEnd == pHead->item || pArrEnd == pEnd)
        {
            // ���ͷŵ����������������һ��������ڴ�, ֱ�Ӱ���һ��������ڴ�ָ����յ��������
            pHead->item = pArrStart;
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
    inline bool query(pointer p) const
    {
        return get_head(p) != nullptr;
    }
    // �����ǰ�ڴ�ص�״̬, ���ÿ���ڴ���״̬
    inline void dump() const
    {
        PMEMORY_HEAD pHead = _Mem;
        int index = 0;
        while (pHead)
        {
            LPBYTE pAllocStart = reinterpret_cast<LPBYTE>(pHead) + sizeof(MEMORY_HEAD);
            int count = static_cast<int>(((pHead->item - pAllocStart) / sizeof(value_type)));
            printf("%03d: �ڴ���׵�ַ 0x%p, �ڴ����ʼ�����ַ: 0x%p, �ڴ��ߴ�: %u, �ѷ��� %d ����Ա, ��ǰ���յ��ڴ����� = 0x%p\n",
                index++, pHead, pAllocStart, (UINT)pHead->size, count, pHead->list);
            pHead = pHead->next;
        }
    }

private:
    // ��������ַ�Ƿ�������ڴ��ĵ�ַ
    inline bool is_head(PMEMORY_HEAD pHead, LPCVOID p) const
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
    inline PMEMORY_HEAD get_head(LPCVOID p) const
    {
        PMEMORY_HEAD pMemNode = _Mem;
        while (pMemNode)
        {
            PMEMORY_HEAD next = pMemNode->next;
            if (is_head(pMemNode, p))
                return pMemNode;
            pMemNode = next;
        }
        return nullptr;
    }
    // ���ڴ�������������Ա��ȥ
    inline pointer alloc_for_item(PMEMORY_HEAD pHead, size_t _Size, bool isClear) const
    {
        LPBYTE pEnd = reinterpret_cast<LPBYTE>(pHead) + pHead->size;
        LPBYTE ptr = pHead->item;
        const size_t offset = _Size * sizeof(value_type);
        if (ptr + offset > pEnd)
            return 0;   // �ڴ治������һ������, ����0

        pHead->item += offset;    // ָ����һ������ĵ�ַ
        if (pHead->item >= pEnd)
        {
            pHead->item = 0;  // �Ѿ����䵽β����, ����Ϊ0
        }

        if (isClear)
            memset(ptr, 0, offset);
        return reinterpret_cast<pointer>(ptr);
    }

    // ��ȡָ���ڵ�ռ�ü�����Ա, �ڵ�Ϊ���򷵻�0, ��Ϊ�����ٷ���1, ���ش���1��ʾ�Ƕ����Ա
    size_t node_get_count(PMEMORY_HEAD pHead, PLIST_NODE node, PLIST_NODE& pNextNode) const
    {
        pNextNode = nullptr;
        if (!node)
            return 0;

        size_t count = 1;  // �ߵ������ʾ�����������нڵ�, �Ƿ��صĳ�Ա������Ϊ1

        // ���������׽ڵ����һ���ڵ㲻Ϊ0, �Ǿͱ�ʾnextҪô���ų�Ա��, Ҫô������һ���ڵ��ַ
        if (node->next != nullptr)
        {
            // next��Ϊ0, �Ǿ���Ҫô���Ա��, Ҫô����һ���ڵ�ָ��
            // pArr[0] == pHead->pFree->next, ��������һ����
            pointer* pArr = reinterpret_cast<pointer*>(node);
            if (is_head(pHead, node->next))
            {
                pNextNode = node->next; // next��ŵ�����һ���ڵ��ַ
            }
            else
            {
                // next��ŵ��ǳ�Ա��
                count = reinterpret_cast<size_t>(pArr[0]);
                pNextNode = reinterpret_cast<PLIST_NODE>(pArr[1]);
            }
        }
        return count;
    }

    struct NODE_MIN_COUNT
    {
        size_t count;       // ���صĽڵ�ռ�õĳ�Ա��
        PLIST_NODE node;    // ���صĽڵ�
        PLIST_NODE next;    // ���ؽڵ����һ���ڵ�
        PLIST_NODE prev;    // ���ؽڵ����һ���ڵ�
    };

    // ���ڴ�������ڵ�������Ա��ȥ, ���ߵ���������Ļ�, ���յ�����϶���Ϊ��
    // �ȱ�����������, �����ĸ��ڵ�պù��ֳ�ȥ, ȡ��Ա��С��ӽ��Ľڵ�����ȥ
    inline pointer alloc_for_list(PMEMORY_HEAD pHead, size_t _Size, bool isClear, size_t stack)
    {

        NODE_MIN_COUNT min_node{};
        if (!list_get_min_node(pHead, _Size, min_node))
            return nullptr; // ��������û�нڵ��㹻����

        // node ����Ҫ�����ȥ���ڴ�, count ��node ռ�õĳ�Ա��
        if (min_node.count == _Size)
        {
            // Ҫ���صĽڵ�պ��㹻�����ȥ, ֱ�ӷ���
            node_set_next(pHead, min_node.prev, min_node.next);
            return reinterpret_cast<pointer>(min_node.node);
        }

        // �ߵ�����ǽڵ���ó�Ա����Ҫ�����ȥ�ĳ�Ա
        // ������ǰ�ڵ�, Ȼ������ȥ

        LPBYTE pNodeStart = reinterpret_cast<LPBYTE>(min_node.node);
        LPBYTE pNodeEnd = pNodeStart + (sizeof(value_type) * _Size);
        auto _List = reinterpret_cast<PLIST_NODE>(pNodeEnd);

        size_t new_count = min_node.count - _Size;

        // �����ȥ��ǰ�ڵ��ֻʣ��һ����Ա
        if (new_count == 1)
        {
            // �����ȥ��ڵ��ʣ��һ����Ա, �Ǿ������Աָ����һ���ڵ�
            _List->next = min_node.next;    // ֻʣһ����Ա��, �Ǿ�ָ����һ���ڵ�
        }
        else
        {
            // �ߵ������ʣ��1����Ա����, �Ǿ͵�һ����Ա��¼ʣ���Ա��, �ڶ�����Աָ����һ���ڵ�
            auto pArr = reinterpret_cast<pointer*>(_List);

            pArr[0] = reinterpret_cast<pointer>(new_count);     // ��һ����Ա��¼ʣ���Ա��
            pArr[1] = reinterpret_cast<pointer>(min_node.next); // �ڶ�����Աָ����һ���ڵ�
        }

        // ����һ���ڵ�ָ�� _List
        node_set_next(pHead, min_node.prev, _List);

        if (isClear)
            memset(pNodeStart, 0, sizeof(value_type) * _Size);
        return reinterpret_cast<pointer>(pNodeStart);
    }

    // ��ȡ�ڵ����Ҫ����ĳ�Ա����ӽ��Ľڵ�, �����ؽڵ�ĳ�Ա��
    // pHead = �ڴ��
    // _Size = Ҫ����ĳ�Ա��
    // ret_node = ���շ��ص�����
    inline bool list_get_min_node(PMEMORY_HEAD pHead, size_t _Size, NODE_MIN_COUNT& ret_node)
    {
        ret_node = { 0 };

        size_t _Count = 0;
        PLIST_NODE prev = nullptr;
        PLIST_NODE node = pHead->list;
        while (node)
        {
            _Count++;
            PLIST_NODE next = nullptr;
            size_t node_count = node_get_count(pHead, node, next);

            // �����ǰ�ڵ㹻����, �Ǿ��ȼ�¼����
            if (node_count >= _Size)
            {
                // ��ǰ�ڵ㹻����, ����С�ĳ�Ա��¼����, Ҫ��ѭ��������û���ҵ����ʵĽڵ�, �Ǿͷ�������ڵ�
                if (ret_node.count == 0 || node_count < ret_node.count)
                {
                    // �Ȱ�����ڵ㱣������, �������һ�������ʵĻᱻ����
                    ret_node.node = node;
                    ret_node.count = node_count;
                    ret_node.next = next;
                    ret_node.prev = prev;
                }
            }

            // ��ǰ�ڵ����Ҫ����ĳ�Ա�պ�һ��, ����Ҫ��������, ֱ�ӷ���
            if (node_count == _Size)
                break;

            // ������һ��ѭ��, �ڵ㲻��������������Ҫ����, ֱ����һ��ѭ���Ϳ���

            prev = node;    // ��¼��һ���ڵ�, ��һ��ѭ����ʱ����һ���ڵ�Ϊ��
            node = next;    // ָ����һ���ڵ�, ����ѭ��
        }

        // ֻҪ ret_node.node �и���ֵ, �Ǿͱ�ʾ�϶��к��ʵĽڵ㷵��
        return ret_node.node != nullptr;
    }

    // ��ȡ������ʹ��ݽ����ĵ�ַ�����Ľڵ�, ��β/ β�� ��������
    // pHead = �ڴ��
    // pStart = Ҫ�жϵ���ʼ��ַ
    // pEnd = Ҫ�жϵĽ�����ַ
    int node_get_memory_adjacent(PMEMORY_HEAD pHead, LPBYTE pStart, LPBYTE pEnd, NODE_MIN_COUNT& ret_node)
    {
        size_t _Count = 0;
        PLIST_NODE prev = nullptr;
        PLIST_NODE node = pHead->list;
        ret_node = { 0 };
        while (node)
        {
            _Count++;

            PLIST_NODE next = nullptr;
            size_t node_count = node_get_count(pHead, node, next);

            LPBYTE pNodeStart = reinterpret_cast<LPBYTE>(node);
            LPBYTE pNodeEnd = pNodeStart + (sizeof(value_type) * node_count);

            int adjacent = is_memory_adjacent(pStart, pEnd, pNodeStart, pNodeEnd);
            if (adjacent)
            {
                ret_node.count = node_count;
                ret_node.node = node;
                ret_node.next = next;
                ret_node.prev = prev;
                return adjacent;
            }

            prev = node;
            node = next;
        }
        return 0;
    }

    // �ж�������β��ַ�Ƿ���������, 1����2 ����1, 2����1 ����-1, ����������0
    int is_memory_adjacent(LPBYTE pStart1, LPBYTE pEnd1, LPBYTE pStart2, LPBYTE pEnd2)
    {
        // �����һ��β��ַ�͵ڶ����׵�ַһ��, �ͱ�ʾ 1 ���� 2
        if (pEnd1 == pStart2)
            return 1;
        
        // ����ڶ���β��ַ�͵�һ���׵�ַһ��, �ͱ�ʾ 2 ���� 1
        if (pEnd2 == pStart1)
            return -1;

        return 0;
    }
    // ����һ���ڴ�, ��������Ҫ������ٸ���Ա
    inline PMEMORY_HEAD malloc_head(size_t count)
    {
        // TODO ����, ÿ���·�����ڴ�Ӧ�÷�����
        const size_t newSize = sizeof(MEMORY_HEAD) + count * sizeof(value_type);
        LPBYTE pStart = _Al.allocate(newSize);
        LPBYTE pEnd = pStart + newSize;

        PMEMORY_HEAD pHead = reinterpret_cast<PMEMORY_HEAD>(pStart);

        pHead->next     = nullptr;
        pHead->size     = newSize;
        pHead->item     = pStart + sizeof(MEMORY_HEAD);
        pHead->list     = nullptr;
        return pHead;
    }


    // TODO ���ͷŵĵ�ַ�ŵ�������, �����������, ÿ�μ��붼�ñ�������λ��, ����ܺϲ����ڵ�ͺϲ�
    // ����ͷŻ�Ӱ���ٶ�, �������ֻ�ܴ��һ��ָ��Ŀռ�, ��������֪����ʲô���ݽṹ����
    // pHead = �ڴ��ͷ�ṹ
    // pFreeStart = �ͷŵ��ڴ濪ʼ��ַ
    // pFreeEnd = �ͷŵ��ڴ������ַ
    // _FreeCount = �ͷŵ��ڴ��Ա��
    inline bool combine_free_pointer(PMEMORY_HEAD pHead, LPBYTE pFreeStart, LPBYTE pFreeEnd, size_t _FreeCount)
    {
        // �������������м��з����ȥ������, ���յ�������
        // ���յ�ʱ�򿴿��ڵ�͵�ǰ���յ��ڴ��ǲ���������, ���������ڵ�/�ڵ���������, �����͵������鱣��
        // ���ڻ��յ�������������ģʽ
        // 1. ��һ�������ڴ����ĵ�ַ, �Ǿ��ǳ�Ա��, ��ʾ�����Ա��������ĳ�Ա�ж��ٸ���������, ���һ����Աָ����һ���ڵ�
        // 2. ��һ����Ա���ڴ����ĵ�ַ, �Ǿ���ָ����һ���ڵ�

        // �ڴ�����β��ַ, �жϻ��յ��ڴ��Ƿ���ڴ�����β��ַһ��, һ���ͱ�ʾ�����ڴ涼��������, ��ԭ����ʼ״̬
        LPBYTE pItemStart = reinterpret_cast<LPBYTE>(pHead) + sizeof(MEMORY_HEAD);
        LPBYTE pItemEnd = pItemStart + pHead->size - sizeof(MEMORY_HEAD);
        if (pFreeStart == pItemStart && (pHead->item == 0 ? pFreeEnd == pItemEnd : pFreeEnd == pHead->item))
        {
            // ���յ��������ڴ�, ��ԭ����ʼ״̬
            pHead->item = pItemStart;
            pHead->list = nullptr;
            return true;
        }

        //////////////////////////////////////////////////////////////////////////
        // ��������, �ҵ������λ��, ���������Ǵ�С���������, �����ҵ���һ���ȵ�ǰ�ڵ��Ľڵ�, �Ͳ��뵽��ǰ�ڵ�ĺ���
        PLIST_NODE prev = nullptr;
        PLIST_NODE node = pHead->list;
        while (node)
        {
            PLIST_NODE next = nullptr;
            size_t node_count = node_get_count(pHead, node, next);

            LPBYTE pNodeStart = reinterpret_cast<LPBYTE>(node);
            LPBYTE pNodeEnd = pNodeStart + (sizeof(value_type) * node_count);


            int adjacent = is_memory_adjacent(pFreeStart, pFreeEnd, pNodeStart, pNodeEnd);
            if (adjacent != 0)
            {
                // ���յ��ڴ� ���� ��ǰ�ڵ�, ���յ��ڴ��ڵ�ǰ�ڵ�����, ������ǰ�ڵ����ʼ��ַ
                if (adjacent > 0)
                    pNodeStart = pFreeStart;
                else
                    pNodeEnd = pFreeEnd;

                size_t newCount = (pNodeEnd - pNodeStart) / sizeof(value_type);
                // ���յ��ڴ�͵�ǰ�ڵ���úϲ�, �ϲ������һ�½ڵ�����, Ȼ��Ϳ��Է�����
                auto pArr = reinterpret_cast<pointer*>(pNodeStart);
                pArr[0] = reinterpret_cast<pointer>(newCount);      // ��һ����Աָ���Ա��
                pArr[1] = reinterpret_cast<pointer>(next);          // �ڶ�����Աָ����һ���ڵ�
                return true;
            }


            // ���յ��ڴ� �� ��ǰ�ڵ� ������, �ж��Ƿ��Ǽ��뵱ǰλ��
            if (pFreeStart > pNodeEnd)
            {
                // ���յĵ�ַ�ڵ�ǰ�ڵ���ұ�, ���뵽�ڵ�ĺ���, Ȼ�󷵻�
                PLIST_NODE free_node = node_make(pFreeStart, pFreeStart);
                node_set_next(pHead, node, free_node);  // ��ǰ�ڵ����һ���ڵ���ǻ��շ��ڴ�
                node_set_next(pHead, free_node, next);  // ���յ��ڴ����һ���ڵ���ǵ�ǰ�ڵ����һ���ڵ�
                return true;
            }
            
            // ����������, ����߳�ѭ��, �Ǿ���Ҫ���뵽����β��
            prev = node;
            node = next;
        }

        PLIST_NODE free_node = node_make(pFreeStart, pFreeStart);
        node_set_next(pHead, prev, free_node);  // ֱ�Ӽ��뵽����β��, �߳�ѭ��, prev�������һ���ڵ�
        return true;
    }

    // ��һ���ڴ�֮������һ���ڵ�
    PLIST_NODE node_make(LPBYTE pStart, LPBYTE pEnd)
    {
        size_t count = (pEnd - pStart) / sizeof(value_type);
        PLIST_NODE pNode = reinterpret_cast<PLIST_NODE>(pStart);
        node_set_value(pNode, count, nullptr);
        return pNode;
    }

    // �ϲ�����ڵ�, ������������, ���׽ڵ��ʣ�µ����нڵ�Ƚ�, ������û���������ڴ�
    void merge_nodes(PMEMORY_HEAD pHead)
    {
        return; // �ϰ�
        PLIST_NODE first_node = pHead->list;
        PLIST_NODE first_next = nullptr;
        size_t first_count = node_get_count(pHead, first_node, first_next);
        LPBYTE pStart = reinterpret_cast<LPBYTE>(first_node);
        LPBYTE pEnd = pStart + (sizeof(value_type) * first_count);

        // 
        size_t _Count = 0;
        PLIST_NODE prev = first_node;
        PLIST_NODE node = first_next;
        while (node)
        {
            PLIST_NODE next = nullptr;
            size_t node_count = node_get_count(pHead, node, next);

            LPBYTE pNodeStart = reinterpret_cast<LPBYTE>(node);
            LPBYTE pNodeEnd = pNodeStart + (sizeof(value_type) * node_count);
            int adjacent = is_memory_adjacent(pStart, pEnd, pNodeStart, pNodeEnd);

            if (adjacent > 0)
            {
                // ��һ���ڵ����� ��ǰö�ٵĽڵ�, pEnd �ĳ� pNodeEnd
                pEnd = pNodeEnd;
            }
            else if (adjacent < 0)
            {
                // ��ǰö�ٵĽڵ� ���� ��һ���ڵ�
                pStart = pNodeStart;
            }
            if (adjacent)
            {
                // ָ���Ѿ��ϲ���, ����ϲ��ڵ�

                if (node == first_next)
                {
                    // ��ǰ�ڵ��ǵ�һ���ڵ����һ���ڵ�, ����һ���ڵ�ϲ���
                    // ����ðѵ�һ���ڵ����һ���ڵ����һ��
                    first_next = next;
                }
                else
                {
                    // ���ǵ�һ���ڵ����һ���ڵ�, �ϲ�����ǰ�ڵ�
                    // �Ȱѵ�ǰ�ڵ�Ͽ�����, ����һ���ڵ�ָ����һ���ڵ�
                    node_set_next(pHead, prev, next);
                }

                // �ѵ�ǰ�ڵ�Ͽ�����, ����һ���ڵ�ָ����һ���ڵ�
                node_set_next(pHead, prev, next);

            }

            prev = node;
            node = next;
            _Count++;
        }

        size_t new_count = (pEnd - pStart) / sizeof(value_type);
        node_set_value(first_node, new_count, nullptr);
    }

    // ��������, ÿ����һ�������ûص�����ȥ����
    // pHead = �ڴ��ͷ�ṹ
    // _Pred = �ص�����(���սڵ�, ��һ���ڵ�, ��һ���ڵ�, �ڵ��Ա��, �ڵ����), ����0��������, ���ط�0��ֹ����
    template<typename _Pr>
    void list_enum(PMEMORY_HEAD pHead, PLIST_NODE node, _Pr&& _Pred)
    {
        size_t _Count = 0;
        PLIST_NODE prev = nullptr;
        while (node)
        {
            PLIST_NODE next = nullptr;
            size_t node_count = node_get_count(pHead, node, next);

            if (_Pred(node, next, prev, node_count, _Count++))
                break;

            prev = node;
            node = next;
        }

    }

    // �ݹ�ϲ�����������ڵ�, ��������ڵ�ĵ�ַ��������, �ǾͰ���һ���ڴ�ϲ���һ��
    void combine_node(PMEMORY_HEAD pHead, PLIST_NODE pNode1, PLIST_NODE pNode2)
    {
        if (!pNode2 || !pNode1)
            return; // �ǿսڵ�, ����Ҫ�ϲ�

        PLIST_NODE pNext1 = nullptr, pNext2 = nullptr, pFirstNode = nullptr, pNextNode = nullptr;
        const size_t count1 = node_get_count(pHead, pNode1, pNext1);
        const size_t count2 = node_get_count(pHead, pNode2, pNext2);

        LPBYTE pNodeStart1 = reinterpret_cast<LPBYTE>(pNode1);
        LPBYTE pNodeEnd1 = pNodeStart1 + (sizeof(value_type) * count1);

        LPBYTE pNodeStart2 = reinterpret_cast<LPBYTE>(pNode2);
        LPBYTE pNodeEnd2 = pNodeStart2 + (sizeof(value_type) * count2);
        pNextNode = pNext2;

        // ���жϽڵ�2�Ľ�β��ַ�Ƿ�ͽڵ�1����ʼ��ַ����, ������˵�������ڵ���������
        if(pNodeStart1 == pNodeEnd2)
        {
            // �ڵ�2�Ľ�����ַ�ǽڵ�1�Ŀ�ʼ��ַ, �Ǿ��� �ڵ�1���Žڵ�2
            // �׽ڵ�ָ��ڵ�2, ��һ���ڵ���ǽڵ�2����һ���ڵ�
            // ��Ϊ��һ���ڵ�ָ��ľ��ǵڶ����ڵ�, ������һ���ڵ���Զ�ĵڶ����ڵ�ָ�����һ���ڵ�
            pFirstNode = pNode2;
        }
        // ���жϽڵ�1�Ľ�β��ַ�Ƿ�ͽڵ�2����ʼ��ַ����
        else if (pNodeEnd1 == pNodeStart2)
        {
            // �ڵ�1�Ľ�����ַ�ǽڵ�2�Ŀ�ʼ��ַ, �Ǿ��� �ڵ�2���Žڵ�1
            // �׽ڵ㻹�ǽڵ�1
            pFirstNode = pHead->list;
        }
        // ������������ж϶�������, �Ǿͱ�ʾ�������ڵ㲻��������
        else
        {
            // �����ڵ�ĵ�ַ��������, ����Ҫ�ϲ�
            return;
        }
        const size_t newCount = count1 + count2;

        node_set_value(pFirstNode, newCount, pNextNode);

        pHead->list = pFirstNode;          // �׽ڵ�ָ�򱾴λ��յ��ڴ�

        // �ݹ�ϲ������ڵ�ĵ�ַ
        combine_node(pHead, pFirstNode, pNextNode);

    }

    // ���ýڵ����һ���ڵ� pPrevNode->next = pNextNode, ���ж��ǰ����鱣�滹�ǰ���һ���ڵ㱣��
    // �����һ���ڵ�Ϊ��ָ��, �ǾͰ���һ���ڵ㸳ֵ���׽ڵ�
    inline void node_set_next(PMEMORY_HEAD pHead, PLIST_NODE pPrevNode, PLIST_NODE pNextNode)
    {
        if (!pPrevNode)
        {
            pHead->list = pNextNode;
            return;
        }
        // ��Ҫ�ж���һ���ڵ��Ǵ��һ��ָ�뻹�Ƕ����Ա
        PLIST_NODE next = nullptr;
        size_t count = node_get_count(pHead, pPrevNode, next);
        node_set_value(pPrevNode, count, pNextNode);

    }

    // ���ýڵ���Ϣ
    inline void node_set_value(PLIST_NODE node, size_t count, PLIST_NODE next)
    {
        if (count == 1)
        {
            node->next = next;
        }
        else
        {
            pointer* pArr = reinterpret_cast<pointer*>(node);
            pArr[0] = reinterpret_cast<pointer>(count);
            pArr[1] = reinterpret_cast<pointer>(next);
        }
    }

    // ��������ڴ����м��ַ, ��������������, һ���Ǽ�¼�м�����ĵ�ַ, һ���Ǽ�¼�м����ҵĵ�ַ
    // ���������ڴ��ʱ����Կ�һ��, ��ȡ�м��ַ, Ȼ���ж��Ƿŵ���߻����ұ�
    inline pointer get_mid_address(PMEMORY_HEAD pHead)
    {
        LPBYTE pStart = reinterpret_cast<LPBYTE>(pHead) + sizeof(MEMORY_HEAD);
        LPBYTE pEnd = (pStart + pHead->size - sizeof(MEMORY_HEAD));
        size_t count = (pEnd - pStart) / sizeof(value_type);
        size_t mid = count / 2;
        return (pointer)(pStart + mid * sizeof(value_type));
    }
    
};

#if CMEMORYPOOL_ISDEBUG == 0
template<class _Ty, class _Alloc>
#endif
class CMemoryPoolView
{
    using value_type = _Ty;
#if CMEMORYPOOL_ISDEBUG
    using MEMPOOL = CMemoryObjectPool;
#else
    using MEMPOOL = CMemoryObjectPool<value_type, _Alloc>;
#endif
    MEMPOOL* pool;
public:

    CMemoryPoolView(MEMPOOL* pool = 0) : pool(pool)
    {

    }
    void init(MEMPOOL* pool)
    {
        this->pool = pool;
    }

    // ��ȡ�������ڴ�, ������������������ڴ�, ֻ��¼�����ڴ����������ַ��ʣ��Ŀɷ����ڴ�
    int GetItemSize()
    {
        size_t size = 0;
        PMEMORY_HEAD pMemNode = pool->_Mem;
        while (pMemNode)
        {
            LPBYTE pEnd = (LPBYTE)pMemNode + pMemNode->size;
            if (pMemNode->item)
                size += (pEnd - (LPBYTE)pMemNode->item);
            pMemNode = pMemNode->next;
        }
        return (int)size;
    }
    // ��ȡ���յ��ڴ��ܳߴ�, ���ص�λΪ�ֽ�
    int GetFreeListSize(int& listCount)
    {
        listCount = 0;
        auto pfn_get_list_size = [&](PMEMORY_HEAD pHead) -> size_t
        {
            size_t count = 0;
            PLIST_NODE list = pHead->list;
            while (list)
            {
                listCount++;
                PLIST_NODE next = nullptr;
                count += pool->node_get_count(pHead, list, next);
                if (next == nullptr)
                    break;
                list = next;
            }
            return count * sizeof(value_type);
        };

        size_t size = 0;
        PMEMORY_HEAD pMemNode = pool->_Mem;
        while (pMemNode)
        {
            size += pfn_get_list_size(pMemNode);
            pMemNode = pMemNode->next;
        }
        return (int)size;
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
        return (int)pool->node_get_count(pHead, node, pNextNode);
    }
    // ��ȡ��ʼ�����ڴ����ʼ��ַ�ͽ�����ַ, �����ж��ٸ���Ա
    int GetItemStartEnd(PMEMORY_HEAD pHead, LPBYTE& pStart, LPBYTE& pEnd)
    {
        pStart = (LPBYTE)pHead;
        pEnd = pStart + pHead->size;
        pStart = pStart + sizeof(MEMORY_HEAD);
        return (int)((pEnd - pStart) / sizeof(value_type));
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
        LPBYTE p = (LPBYTE)ptr;
        if (p < begin || p >= end)
            return -1;

        const size_t offset = p - begin;
        if (offset % sizeof(value_type) != 0)
            return -1;

        return (int)(offset / sizeof(value_type));
    };

    // ��⴫��ĵ�ַ�Ƿ��ǻ���������ĵ�ַ
    bool IsFreeList(PMEMORY_HEAD pHead, LPCVOID ptr)
    {
        PLIST_NODE node = pHead->list;
        while (node)
        {
            PLIST_NODE pNextNode = nullptr;

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
    int IsAllocated(PMEMORY_HEAD pHead, LPCVOID ptr)
    {
        // ����һ���ж������ַ�Ƿ�Ϸ�, ������Ϸ�, ������������
        // ����౾��Ͳ���Ϊ��Ч��, ����Ϊ�˾��Ե�׼ȷ, �����������һ���жϵ�ַ�Ƿ�Ϸ�
        PointerToIndex(pHead, ptr);

        // �׵�ַ������Ҫ�����ȥ�ĵ�ַ, ������ַ����һ������ĵ�ַ
        // �����һ������ĵ�ַΪ��, �Ǿ���ָ������ڴ�Ľ�����ַ
        LPBYTE pStart = reinterpret_cast<LPBYTE>(pHead) + sizeof(MEMORY_HEAD);
        LPBYTE pEnd = pHead->item ? pHead->item : (pStart + pHead->size - sizeof(MEMORY_HEAD));
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


