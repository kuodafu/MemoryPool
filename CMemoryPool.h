#pragma once
#include "CMemoryAllocator.h"
NAMESPACE_MEMORYPOOL_BEGIN

#if CMEMORYPOOL_ISDEBUG == 0
template<class _Alloc = CMemoryPoolAllocator<BYTE>>
#endif
// 变长内存池, 目前是打算以定长内存池为基础, 每次分配4/8的倍数字节
// 

// 先不弄了, 有空再继续
class CMemoryPool
{
#if CMEMORYPOOL_ISDEBUG
    using _Alloc = std::allocator<BYTE>;
#endif

private:


    typedef struct LIST_NODE
    {
        LIST_NODE* next;     // 下一个节点
    }*PLIST_NODE;
    typedef struct MEMORY_HEAD
    {
        MEMORY_HEAD*    next;       // 下一个块分配的内存
        size_t          size;       // 这一块内存占用的字节
        PLIST_NODE      pFree;      // 回收回来的内存, 如果都没有回收, 那整个值就是0, 数组也分配完了就需要开辟新的内存块
        LPBYTE          arr;        // 分配出去的内存, 每次分配出去都指向下一个成员, 直到越界后就从链表中取下一个节点
    }*PMEMORY_HEAD;

    _Alloc              _Al;        // 分配器, 分配内存都是按字节分配, 自己计算分配成员需要多少字节 + 头部结构
    PMEMORY_HEAD        _Mem;       // 内存块头结构, 分配和回收都根据这个结构来操作
    PMEMORY_HEAD        _Now;       // 当前操作的内存块
    PMEMORY_HEAD        _Arr[16];   // 存放分配内存的内存块数组, 每个块分配的内存尺寸 = 4,8,12,16,20,24,28,32,40,48,56,64,80,96,112,128


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
    // 清空已经申请的内存, 不会释放, 已经分配出去的内存都不再可用
    // 清空后下次从头开始分配内存
    inline void Clear()
    {
        if (!_Mem)
            return;

        // 把所有内存块都恢复到刚申请的状态

        PMEMORY_HEAD pHead = _Mem;
        _Now = _Mem;    // 当前操作的内存块指向第一个内存块
        while (pHead)
        {
            // arr指向第一个分配出去的内存, 重置为初始状态
            pHead->arr = reinterpret_cast<LPBYTE>(pHead) + sizeof(MEMORY_HEAD);
            pHead->pFree = 0;   // 没有回收的内存

            pHead = pHead->next;
        }
    }
public:
    // 初始化内存池, 内部会申请空间, 如果不初始化, 在分配内存的时候会自动初始化, 默认尺寸是1M尺寸
    inline bool init(size_t size = 0x100000)
    {
        if (_Mem)
            return true;

        _Mem = malloc_head(size);
        _Now = _Mem;        // 当前操作的内存块
        return true;
    }


    // 申请失败则抛出int类型异常
    // 异常值 1=初始化失败, 2=空间不足,需要释放一些内存, 有抛出异常表示有严重的内存泄漏.....
    inline LPVOID malloc(bool isClear = false)
    {
        if (!_Mem)
            init();

        if (!_Mem)  // 初始化失败, 应该不会走这里, 初始化失败就是分配内存失败, 已经抛出异常了
            throw std::bad_alloc();


        PMEMORY_HEAD pHead = _Now;  // 从这个内存块开始分配

        do
        {
            // 先从数组里分配, 如果数组分配完了, 就从链表里分配
            if (pHead->arr)
                return alloc_for_arr(pHead, isClear);

            // 没有值, 那就是没有空闲的内存, 从回收的内存中取
            if (pHead->pFree)
                return alloc_for_listnode(pHead, isClear);


            // 走到这里就是没有余额也没有回收的内存, 申请新的内存
            // 从下一块内存开始分配, 如果没有下一块内存, 就申请新的内存


            if (pHead->next)
            {
                // 还有下一块内存, 从下一块内存继续分配, 当前内存块指向下一个内存块
                pHead = pHead->next;
                _Now = pHead;
                continue;
            }

            // 走到这里就是当前内存块没有下一块内存了, 枚举一下所有内存块, 如果有空闲的内存块, 就从空闲的内存块开始分配
            if (pHead != _Mem)
            {
                PMEMORY_HEAD pMemNode = _Mem;
                while (pMemNode)
                {
                    if (pMemNode->arr)
                    {
                        // 数组有值? 这个有值那应该就是bug了
#ifdef _DEBUG
                        __debugbreak();
#endif
                        return alloc_for_arr(pMemNode, isClear);
                    }

                    if (pMemNode->pFree)
                    {
                        return alloc_for_listnode(pMemNode, isClear);
                    }

                    // 数组里没有可分配的, 这个应该是必然的, 只有数组里没有分配的才会使用其他内存块分配
                    // 回收的内存里也没有可分配的, 那就是没有可用的内存了, 执行下一个内存块继续分配

                    pMemNode = pMemNode->next;
                }
                // 如果上面的循环没有返回, 那就是没有可用的内存了, 需要申请新的内存
            }


            // 走到这里就是没有下一块内存, 也没有可用内存分配, 需要申请一块新的内存
            // pHead 已经指向最后一个内存块了, 

            // TODO 待定, 每次新分配的内存应该分配多大
            const size_t newCount = pHead->size * 2;


            _Now = malloc_head(newCount);   // 当前操作的内存块指向新分配的内存块
            pHead->next = _Now;             // 新分配的内存块挂在链表的最后
            pHead = _Now;                   // pHead指向新分配的内存块, 继续循环分配内存

        } while (true);

        // 能走到这里的肯定就是没有正确的分配内存出去, 抛出个分配内存异常
        throw std::bad_alloc();
        return 0;
    }

    inline bool free(LPVOID p)
    {
        PMEMORY_HEAD pHead = GetHead(p);
        if (!pHead)
        {
            throw std::exception(__FUNCTION__ ": 传递进来了不是内存池里的地址", 0);
            return false;
        }

        //UNDONE 这里可能需要处理64位指针的问题, 如果不处理的话, 那64位最小分配出去的块是8字节, 32位是4字节
        // 如果回收的内存不足sizeof(LPVOID)的话, 那这套代码就会出错
        // node->next = xxx; 这里就会溢出, 如果溢出的位置是已经分配出去的内存, 那就会出现内存污染
        // 如果溢出出去的是其他内存, 那有可能会崩溃

        // 释放就把回收的内存块放入到链表里, 下次分配的时候就从链表里取

        PLIST_NODE node = reinterpret_cast<PLIST_NODE>(p);
        node->next = pHead->pFree; // 回收的内存当成下一个被分配出去的内存, 指向原来的链表头
        pHead->pFree = node;         // 当前的链表头指向本次回收的内存

        //TODO 这里是否可以设置成 当前内存块指向回收回来这个内存所在的块, 这样下次分配的时候就可以从这个块里分配
        _Now = pHead;
        return true;
    }

    // 查询地址是否是内存池里的地址
    inline bool query(LPVOID p) const
    {
        return GetHead(p) != nullptr;
    }

private:
    // 返回这个地址对应的内存块结构, 如果这个地址不是内存池的地址, 返回0
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
                return pMemNode;    // 地址大于等于首地址, 小于结束地址, 传入的是内存池里的地址, 返回内存块结构地址

            pMemNode = next;
        }
        return 0;
    }
    // 从内存块的数组里分配成员出去
    inline LPVOID alloc_for_arr(PMEMORY_HEAD pHead, bool isClear) const
    {
        LPBYTE pEnd = reinterpret_cast<LPBYTE>(pHead) + pHead->size;
        LPBYTE ptr = pHead->arr;
        pHead->arr;    // 执行下一个分配的地址
        if (pHead->arr >= pEnd)
        {
            pHead->arr = 0;  // 已经分配到尾部了, 重置为0
        }

        //if (isClear)
        //    memset(ptr, 0, sizeof(value_type));
        return ptr;
    }

    // 从内存块的链表节点里分配成员出去
    inline LPVOID alloc_for_listnode(PMEMORY_HEAD pHead, bool isClear) const
    {
        LPBYTE pEnd = reinterpret_cast<LPBYTE>(pHead) + pHead->size;
        PLIST_NODE pNode = pHead->pFree; // 把头节点分配出去
        pHead->pFree = pNode->next;      // 头节点指向下一个节点
        if (isClear)
            memset(pNode, 0, sizeof(LIST_NODE));
        return pNode;
    }

    // 分配一块内存, 参数是需要分配多少个成员
    inline PMEMORY_HEAD malloc_head(size_t count)
    {
        // TODO 待定, 每次新分配的内存应该分配多大
        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);

        // 页对齐
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



    // 根据传递进来的尺寸, 找出这个尺寸对应的数组下标, 返回对应的数组的链表, 链表就是要分配的内存, 不会返回0
    inline int GetListNode(size_t size)
    {
        const size_t arr[] = { 4, 8, 12, 16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128 };
        // 二分查找, 找出第一个大于等于size的值
        int left = 0;
        int right = sizeof(arr) / sizeof(arr[0]) - 1;
        int nPos = -1;
        if (size < arr[0])
            nPos = 0;
        else if(size > arr[right])
            nPos = -2;
        else
        {
            // 不小于第一个成员, 且不大于最后一个成员, 那就在中间寻找
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
            __debugbreak(); // 走到这里能是-1那就是bug了
        }

        if (nPos >= 0)
        {
            PMEMORY_HEAD pHead = _Arr[nPos];
            if (!pHead)
            {
                // 没有这个尺寸的内存块, 需要新建一个
                pHead = malloc_head(arr[nPos]);
                _Arr[nPos] = pHead;
            }
        }

        return nPos;
    }

};


NAMESPACE_MEMORYPOOL_END


