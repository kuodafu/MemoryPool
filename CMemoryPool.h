#pragma once
#include "CMemoryAllocator.h"
NAMESPACE_MEMORYPOOL_BEGIN

#if CMEMORYPOOL_ISDEBUG == 0
template<class _Alloc = CMemoryPoolAllocator<BYTE>>
#endif
// �䳤�ڴ��, Ŀǰ�Ǵ����Զ����ڴ��Ϊ����, ÿ�η���4/8�ı����ֽ�
// 

// �Ȳ�Ū��, �п��ټ���
class CMemoryPool
{
#if CMEMORYPOOL_ISDEBUG
    using _Alloc = std::allocator<BYTE>;
#endif

private:


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

    _Alloc              _Al;        // ������, �����ڴ涼�ǰ��ֽڷ���, �Լ���������Ա��Ҫ�����ֽ� + ͷ���ṹ
    PMEMORY_HEAD        _Mem;       // �ڴ��ͷ�ṹ, ����ͻ��ն���������ṹ������
    PMEMORY_HEAD        _Now;       // ��ǰ�������ڴ��
    PMEMORY_HEAD        _Arr[16];   // ��ŷ����ڴ���ڴ������, ÿ���������ڴ�ߴ� = 4,8,12,16,20,24,28,32,40,48,56,64,80,96,112,128


public:
    CMemoryPool() :_Mem(0), _Al(), _Now(0), _Arr{ 0 }
    {

    }
    explicit CMemoryPool(size_t size) :CMemoryPool()
    {
        init(size);
    }
    CMemoryPool(const CMemoryPool& other) = delete;
    CMemoryPool& operator=(const CMemoryPool& other) = delete;

    CMemoryPool(CMemoryPool&& other) noexcept : CMemoryPool()
    {
        _Al = std::move(other._Al);
        _Mem = other._Mem;
        _Now = other._Now;

        other._Mem = 0;
        other._Now = 0;
    }

    ~CMemoryPool()
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
    }
    // ����Ѿ�������ڴ�, �����ͷ�, �Ѿ������ȥ���ڴ涼���ٿ���
    // ��պ��´δ�ͷ��ʼ�����ڴ�
    inline void Clear()
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
    // ��ʼ���ڴ��, �ڲ�������ռ�, �������ʼ��, �ڷ����ڴ��ʱ����Զ���ʼ��, Ĭ�ϳߴ���1M�ߴ�
    inline bool init(size_t size = 0x100000)
    {
        if (_Mem)
            return true;

        _Mem = malloc_head(size);
        _Now = _Mem;        // ��ǰ�������ڴ��
        return true;
    }


    // ����ʧ�����׳�int�����쳣
    // �쳣ֵ 1=��ʼ��ʧ��, 2=�ռ䲻��,��Ҫ�ͷ�һЩ�ڴ�, ���׳��쳣��ʾ�����ص��ڴ�й©.....
    inline LPVOID malloc(bool isClear = false)
    {
        if (!_Mem)
            init();

        if (!_Mem)  // ��ʼ��ʧ��, Ӧ�ò���������, ��ʼ��ʧ�ܾ��Ƿ����ڴ�ʧ��, �Ѿ��׳��쳣��
            throw std::bad_alloc();


        PMEMORY_HEAD pHead = _Now;  // ������ڴ�鿪ʼ����

        do
        {
            // �ȴ����������, ��������������, �ʹ����������
            if (pHead->arr)
                return alloc_for_arr(pHead, isClear);

            // û��ֵ, �Ǿ���û�п��е��ڴ�, �ӻ��յ��ڴ���ȡ
            if (pHead->pFree)
                return alloc_for_listnode(pHead, isClear);


            // �ߵ��������û�����Ҳû�л��յ��ڴ�, �����µ��ڴ�
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
                    if (pMemNode->arr)
                    {
                        // ������ֵ? �����ֵ��Ӧ�þ���bug��
#ifdef _DEBUG
                        __debugbreak();
#endif
                        return alloc_for_arr(pMemNode, isClear);
                    }

                    if (pMemNode->pFree)
                    {
                        return alloc_for_listnode(pMemNode, isClear);
                    }

                    // ������û�пɷ����, ���Ӧ���Ǳ�Ȼ��, ֻ��������û�з���ĲŻ�ʹ�������ڴ�����
                    // ���յ��ڴ���Ҳû�пɷ����, �Ǿ���û�п��õ��ڴ���, ִ����һ���ڴ���������

                    pMemNode = pMemNode->next;
                }
                // ��������ѭ��û�з���, �Ǿ���û�п��õ��ڴ���, ��Ҫ�����µ��ڴ�
            }


            // �ߵ��������û����һ���ڴ�, Ҳû�п����ڴ����, ��Ҫ����һ���µ��ڴ�
            // pHead �Ѿ�ָ�����һ���ڴ����, 

            // TODO ����, ÿ���·�����ڴ�Ӧ�÷�����
            const size_t newCount = pHead->size * 2;


            _Now = malloc_head(newCount);   // ��ǰ�������ڴ��ָ���·�����ڴ��
            pHead->next = _Now;             // �·�����ڴ�������������
            pHead = _Now;                   // pHeadָ���·�����ڴ��, ����ѭ�������ڴ�

        } while (true);

        // ���ߵ�����Ŀ϶�����û����ȷ�ķ����ڴ��ȥ, �׳��������ڴ��쳣
        throw std::bad_alloc();
        return 0;
    }

    inline bool free(LPVOID p)
    {
        PMEMORY_HEAD pHead = GetHead(p);
        if (!pHead)
        {
            throw std::exception(__FUNCTION__ ": ���ݽ����˲����ڴ����ĵ�ַ", 0);
            return false;
        }

        //UNDONE ���������Ҫ����64λָ�������, ���������Ļ�, ��64λ��С�����ȥ�Ŀ���8�ֽ�, 32λ��4�ֽ�
        // ������յ��ڴ治��sizeof(LPVOID)�Ļ�, �����״���ͻ����
        // node->next = xxx; ����ͻ����, ��������λ�����Ѿ������ȥ���ڴ�, �Ǿͻ�����ڴ���Ⱦ
        // ��������ȥ���������ڴ�, ���п��ܻ����

        // �ͷžͰѻ��յ��ڴ����뵽������, �´η����ʱ��ʹ�������ȡ

        PLIST_NODE node = reinterpret_cast<PLIST_NODE>(p);
        node->next = pHead->pFree; // ���յ��ڴ浱����һ���������ȥ���ڴ�, ָ��ԭ��������ͷ
        pHead->pFree = node;         // ��ǰ������ͷָ�򱾴λ��յ��ڴ�

        //TODO �����Ƿ�������ó� ��ǰ�ڴ��ָ����ջ�������ڴ����ڵĿ�, �����´η����ʱ��Ϳ��Դ�����������
        _Now = pHead;
        return true;
    }

    // ��ѯ��ַ�Ƿ����ڴ����ĵ�ַ
    inline bool query(LPVOID p) const
    {
        return GetHead(p) != nullptr;
    }

private:
    // ���������ַ��Ӧ���ڴ��ṹ, ��������ַ�����ڴ�صĵ�ַ, ����0
    inline PMEMORY_HEAD GetHead(LPCVOID p) const
    {
        LPBYTE ptr = reinterpret_cast<LPBYTE>(const_cast<LPVOID>(p));
        PMEMORY_HEAD pMemNode = _Mem;
        while (pMemNode)
        {
            PMEMORY_HEAD next = pMemNode->next;
            LPBYTE pStart = reinterpret_cast<LPBYTE>(pMemNode) + sizeof(MEMORY_HEAD);
            LPBYTE pEnd = pStart + pMemNode->size - sizeof(MEMORY_HEAD);
            if (ptr >= pStart && ptr <= pEnd)
                return pMemNode;    // ��ַ���ڵ����׵�ַ, С�ڽ�����ַ, ��������ڴ����ĵ�ַ, �����ڴ��ṹ��ַ

            pMemNode = next;
        }
        return 0;
    }
    // ���ڴ�������������Ա��ȥ
    inline LPVOID alloc_for_arr(PMEMORY_HEAD pHead, bool isClear) const
    {
        LPBYTE pEnd = reinterpret_cast<LPBYTE>(pHead) + pHead->size;
        LPBYTE ptr = pHead->arr;
        pHead->arr;    // ִ����һ������ĵ�ַ
        if (pHead->arr >= pEnd)
        {
            pHead->arr = 0;  // �Ѿ����䵽β����, ����Ϊ0
        }

        //if (isClear)
        //    memset(ptr, 0, sizeof(value_type));
        return ptr;
    }

    // ���ڴ�������ڵ�������Ա��ȥ
    inline LPVOID alloc_for_listnode(PMEMORY_HEAD pHead, bool isClear) const
    {
        LPBYTE pEnd = reinterpret_cast<LPBYTE>(pHead) + pHead->size;
        PLIST_NODE pNode = pHead->pFree; // ��ͷ�ڵ�����ȥ
        pHead->pFree = pNode->next;      // ͷ�ڵ�ָ����һ���ڵ�
        if (isClear)
            memset(pNode, 0, sizeof(LIST_NODE));
        return pNode;
    }

    // ����һ���ڴ�, ��������Ҫ������ٸ���Ա
    inline PMEMORY_HEAD malloc_head(size_t count)
    {
        // TODO ����, ÿ���·�����ڴ�Ӧ�÷�����
        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);

        // ҳ����
        const size_t nPage = count / sysInfo.dwPageSize;
        const size_t newSize = nPage * sysInfo.dwPageSize + (count % sysInfo.dwPageSize == 0 ? 0 : sysInfo.dwPageSize);

        LPBYTE pStart = _Al.allocate(newSize);
        LPBYTE pEnd = pStart + newSize;

        PMEMORY_HEAD pHead = reinterpret_cast<PMEMORY_HEAD>(pStart);

        pHead->next = 0;
        pHead->size = newSize;
        pHead->arr = pStart + sizeof(MEMORY_HEAD);
        pHead->pFree = 0;

        return pHead;
    }



    // ���ݴ��ݽ����ĳߴ�, �ҳ�����ߴ��Ӧ�������±�, ���ض�Ӧ�����������, �������Ҫ������ڴ�, ���᷵��0
    inline int GetListNode(size_t size)
    {
        const size_t arr[] = { 4, 8, 12, 16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128 };
        // ���ֲ���, �ҳ���һ�����ڵ���size��ֵ
        int left = 0;
        int right = sizeof(arr) / sizeof(arr[0]) - 1;
        int nPos = -1;
        if (size < arr[0])
            nPos = 0;
        else if(size > arr[right])
            nPos = -2;
        else
        {
            // ��С�ڵ�һ����Ա, �Ҳ��������һ����Ա, �Ǿ����м�Ѱ��
            while (left <= right)
            {
                int mid = left + (right - left) / 2;
                if (arr[mid] >= size)
                {
                    nPos = mid;
                    right = mid - 1;
                }
                else
                {
                    left = mid + 1;
                }
            }
        }
        if (nPos == -1)
        {
            __debugbreak(); // �ߵ���������-1�Ǿ���bug��
        }

        if (nPos >= 0)
        {
            PMEMORY_HEAD pHead = _Arr[nPos];
            if (!pHead)
            {
                // û������ߴ���ڴ��, ��Ҫ�½�һ��
                pHead = malloc_head(arr[nPos]);
                _Arr[nPos] = pHead;
            }
        }

        return nPos;
    }

};


NAMESPACE_MEMORYPOOL_END


