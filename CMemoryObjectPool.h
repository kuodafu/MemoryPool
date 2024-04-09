#pragma once
#include "CMemoryAllocator.h"
NAMESPACE_MEMORYPOOL_BEGIN

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


#if CMEMORYPOOL_ISDEBUG
class CMemoryPoolView;
#else
template<class _Ty = LPVOID> class CMemoryPoolView;
#endif

// 定长内存池, 每次分配都是固定大小的内存
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


    _Alloc              _Al;        // 分配器, 分配内存都是按字节分配, 自己计算分配成员需要多少字节 + 头部结构
    PMEMORY_HEAD        _Mem;       // 内存块头结构, 分配和回收都根据这个结构来操作
    PMEMORY_HEAD        _Now;       // 当前操作的内存块

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
    // 清空已经申请的内存, 不会释放, 已经分配出去的内存都不再可用
    // 清空后下次从头开始分配内存
    inline void clear()
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
    // 初始化内存池, 内部会申请空间
    inline bool init(size_t size = 0x1000)
    {
        if (_Mem)
            return true;

        _Mem = malloc_head(size);
        _Now = _Mem;        // 当前操作的内存块
        return true;
    }

    // 申请一个成员, 申请失败则抛出 std::bad_alloc 类型异常
    inline pointer malloc(bool isClear = false)
    {
        return malloc_arr(1, isClear);
    }


    // 申请一个数组, 释放内存请使用 free_arr() 释放, 否则只会释放第一个成员
    // 申请失败则抛出 std::bad_alloc 类型异常
    // _Size = 申请多少个成员
    inline pointer malloc_arr(int _Size, bool isClear = false)
    {
        if (_Size < 0 || _Size > 0x7fffffff)
            _Size = 1;

        if (!_Mem)
            init();

        if (!_Mem)  // 初始化失败, 应该不会走这里, 初始化失败就是分配内存失败, 已经抛出异常了
            throw std::bad_alloc();
        

        PMEMORY_HEAD pHead = _Now;  // 从这个内存块开始分配
        pointer pRet = 0;

        auto pfn_ret = [isClear, _Size, &pRet, this](PMEMORY_HEAD pHead) -> bool
        {
            // 先从数组里分配, 如果数组分配完了, 就从链表里分配
            if (pHead->arr)
                pRet = alloc_for_arr(pHead, _Size, isClear);
            
            // 没有值, 那就是没有空闲的内存, 从回收的内存中取
            if (!pRet && pHead->pFree)
                pRet = alloc_for_listnode(pHead, _Size, isClear);
            


            return pRet != 0;
        };


        do
        {
            // 从指定内存块里分配内存
            if (pfn_ret(pHead))
                return pRet;


            // 走到这里就是当前内存块没有剩余内存可分配, 需要从下一块内存开始分配
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
                    // 跳过当前内存块
                    if (_Now != pMemNode && pfn_ret(pMemNode))
                        return pRet;
                    pMemNode = pMemNode->next;
                }
                // 如果上面的循环没有返回, 那就是没有可用的内存了, 需要申请新的内存
            }


            // 走到这里就是没有下一块内存, 也没有可用内存分配, 需要申请一块新的内存
            // pHead 已经指向最后一个内存块了, 

            // TODO 待定, 每次新分配的内存应该分配多大
            const size_t oldCount = (pHead->size - sizeof(MEMORY_HEAD)) / sizeof(value_type);
            const size_t scaleCount = oldCount * 2; // 每次分配的内存是上一次的2倍
            const size_t newCount = (_Size + scaleCount);   // 一定要保证这次分配能存放这次申请的内存大小


            _Now = malloc_head(newCount);   // 当前操作的内存块指向新分配的内存块
            pHead->next = _Now;             // 新分配的内存块挂在链表的最后
            pHead = _Now;                   // pHead指向新分配的内存块, 继续循环分配内存

        } while (true);

        // 能走到这里的肯定就是没有正确的分配内存出去, 抛出个分配内存异常
        throw std::bad_alloc();
        return 0;
    }

    // 重新分配数组, 会把原来的数据拷贝到新的内存里
    // pArr = 需要重新分配的数组地址
    // _OldSize = 原来的数组成员数
    // _Size = 新数组成员数
    inline pointer realloc_arr(pointer pArr, size_t _OldSize, size_t _Size)
    {
        PMEMORY_HEAD pHead = GetHead(pArr);
        if (!pHead)
        {
            throw std::exception(__FUNCTION__ ": 传递进来了不是内存池里的地址", 0);
            return 0;
        }

        pointer pRet = 0;
        LPBYTE pEnd = reinterpret_cast<LPBYTE>(pHead) + pHead->size;
        LPBYTE pArrStart = reinterpret_cast<LPBYTE>(pArr);
        LPBYTE pArrEnd = pArrStart + sizeof(value_type) * _OldSize;
        bool isFree = true;
        if (pArrEnd == pHead->arr || pArrEnd == pEnd)
        {
            // 被释放的这个数组是连着下一个分配的内存, 直接把下一个分配的内存指向回收的这个数组
            pHead->arr = pArrStart;
            pRet = alloc_for_arr(pHead, _Size, false);
            if (pRet)
                return pRet;    // 还够存放, 那就只调整指向位置, 不拷贝内存
            isFree = false;     // pHead->arr = pArrStart; 这一行已经释放了, 把变量设置为假, 后面不需要继续释放
        }

        // 走到这里就是需要重新分配一块数组, 然后拷贝原来的数据到新的内存里
        if (isFree)
        {
            // 先尝试从当前内存块里分配, 如果分配失败就从所有内存块里分配
            pRet = alloc_for_arr(pHead, _Size, false);
        }

        if (!pRet)
            pRet = malloc_arr(_Size, false);

        const size_t newSize = min(_OldSize, _Size);
        memcpy(pRet, pArr, sizeof(value_type) * newSize);

        if (isFree)
        {
            // 然后回收原来的数组
            combine_free_pointer(pHead, pArrStart, pArrEnd, _OldSize);
        }
        return pRet;
    }

    // 释放数组, 调用 malloc_arr 分配的必须调用这个释放, 不然会有些内存无法再次被分配
    inline bool free_arr(pointer p, size_t _Size)
    {
        PMEMORY_HEAD pHead = GetHead(p);
        if (!pHead)
        {
            throw std::exception(__FUNCTION__ ": 传递进来了不是内存池里的地址", 0);
            return false;
        }

        LPBYTE pArrStart = reinterpret_cast<LPBYTE>(p);
        LPBYTE pArrEnd = pArrStart + sizeof(value_type) * _Size;
        LPBYTE pEnd = reinterpret_cast<LPBYTE>(pHead) + pHead->size;
        if (pArrEnd == pHead->arr || pArrEnd == pEnd)
        {
            // 被释放的这个数组是连着下一个分配的内存, 直接把下一个分配的内存指向回收的这个数组
            pHead->arr = pArrStart;
            return true;
        }

        combine_free_pointer(pHead, pArrStart, pArrEnd, _Size);

        //TODO 这里是否可以设置成 当前内存块指向回收回来这个内存所在的块, 这样下次分配的时候就可以从这个块里分配
        _Now = pHead;
        return true;
    }

    inline bool free(pointer p)
    {
        return free_arr(p, 1);
    }

    // 查询地址是否是内存池里的地址
    inline bool query(_Ty* p) const
    {
        return GetHead(p) != nullptr;
    }
    // 输出当前内存池的状态, 输出每个内存块的状态
    inline void dump() const
    {
        PMEMORY_HEAD pHead = _Mem;
        int index = 0;
        while (pHead)
        {
            LPBYTE pAllocStart = reinterpret_cast<LPBYTE>(pHead) + sizeof(MEMORY_HEAD);
            int count = static_cast<int>(((pHead->arr - pAllocStart) / sizeof(value_type)));
            printf("%03d: 内存块首地址 0x%p, 内存块起始分配地址: 0x%p, 内存块尺寸: %u, 已分配 %d 个成员, 当前回收的内存链表 = 0x%p\n",
                index++, pHead, pAllocStart, (UINT)pHead->size, count, pHead->pFree);
            pHead = pHead->next;
        }
    }

private:
    // 检测这个地址是否是这个内存块的地址
    inline bool IsHead(PMEMORY_HEAD pHead, LPCVOID p) const
    {
        LPBYTE ptr = reinterpret_cast<LPBYTE>(const_cast<LPVOID>(p));

        // 首地址就是需要分配出去的地址, 结束地址是下一个分配的地址
        // 如果下一个分配的地址为空, 那就是指向这块内存的结束地址
        LPBYTE pStart = reinterpret_cast<LPBYTE>(pHead) + sizeof(MEMORY_HEAD);
        //LPBYTE pEnd = pHead->arr ? pHead->arr : (pStart + pHead->size - sizeof(MEMORY_HEAD));
        LPBYTE pEnd = (pStart + pHead->size - sizeof(MEMORY_HEAD));
        if (ptr >= pStart && ptr <= pEnd)
        {
            // 传入的地址必须是按成员尺寸对齐, 如果不对齐那就不是当时分配的地址
            const size_t offset = ptr - pStart;
            if (offset % sizeof(value_type) == 0)
                return true;    // 地址大于等于首地址, 小于结束地址, 传入的是内存池里的地址
        }
        return false;
    }

    // 返回这个地址对应的内存块结构, 如果这个地址不是内存池的地址, 返回0
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
    // 从内存块的数组里分配成员出去
    inline pointer alloc_for_arr(PMEMORY_HEAD pHead, int _Size, bool isClear) const
    {
        LPBYTE pEnd = reinterpret_cast<LPBYTE>(pHead) + pHead->size;
        LPBYTE ptr = pHead->arr;
        const size_t offset = _Size * sizeof(value_type);
        if (ptr + offset > pEnd)
            return 0;   // 内存不够分配一个数组, 返回0

        pHead->arr += offset;    // 指向下一个分配的地址
        if (pHead->arr >= pEnd)
        {
            pHead->arr = 0;  // 已经分配到尾部了, 重置为0
        }

        if (isClear)
            memset(ptr, 0, offset);
        return reinterpret_cast<pointer>(ptr);
    }

    // 获取指定节点占用几个成员, 节点为空则返回0, 不为空最少返回1, 返回大于1表示是多个成员
    int GetNodeCount(PMEMORY_HEAD pHead, PLIST_NODE node, PLIST_NODE& pNextNode) const
    {
        pNextNode = 0;
        if (!node)
            return 0;

        int count = 1;  // 走到这里表示回收链表里有节点, 那返回的成员数最少为1

        // 回收链表首节点的下一个节点不为0, 那就表示next要么存着成员数, 要么存着下一个节点地址
        if (node->next != 0)
        {
            // next不为0, 那就是要么存成员数, 要么存下一个节点指针
            // pArr[0] == pHead->pFree->next, 这两个是一样的
            LPINT* pArr = reinterpret_cast<LPINT*>(node);
            if (IsHead(pHead, node->next))
            {
                pNextNode = node->next; // next存放的是下一个节点地址
            }
            else
            {
                // next存放的是成员数
                count = reinterpret_cast<int>(pArr[0]);
                pNextNode = reinterpret_cast<PLIST_NODE>(pArr[1]);
            }
        }
        return count;
    }

    // 获取回收链表首个节点的属性, 返回0表示没有下一个节点
    // 如果返回1表示首个节点存放的是下一个节点的地址
    // 返回大于1表示首节点存放的是一个数组, 返回成员数
    inline int get_first_node_count(PMEMORY_HEAD pHead, PLIST_NODE& pNextNode) const
    {
        pNextNode = 0;
        if (!pHead->pFree)
            return 0;
        return GetNodeCount(pHead, pHead->pFree, pNextNode);
    }
    // 从内存块的链表节点里分配成员出去, 能走到这个方法的话, 回收的链表肯定不为空
    inline pointer alloc_for_listnode(PMEMORY_HEAD pHead, int _Size, bool isClear)
    {
        LPBYTE pStart = reinterpret_cast<LPBYTE>(pHead) + sizeof(MEMORY_HEAD);
        LPBYTE pEnd = pStart + pHead->size - sizeof(MEMORY_HEAD);

        // 首个节点是否是内存块里的地址, 如果不是, 那就表示是这个节点有多少个成员是连续的
        pointer* pArr = reinterpret_cast<pointer*>(pHead->pFree);
        if (!pArr)
            return 0;

        PLIST_NODE pNextNode = 0;
        int count = get_first_node_count(pHead, pNextNode);

        if (_Size == 1 && count == 1)
        {
            // 只分配一个成员, 并且首个节点不是成员数, 那就直接把这个节点分配出去, 然后指向下一个节点
            PLIST_NODE pNode = pHead->pFree; // 把头节点分配出去
            pHead->pFree = pNode->next;      // 头节点指向下一个节点
            if (isClear)
                memset(pNode, 0, sizeof(value_type));
            return reinterpret_cast<pointer>(pNode);
        }

        if (count == 1)
        {
            //TODO 走到这里就是要分配多个成员, 但是回收的链表首个成员不是成员数, 这里是否有必要遍历链表分配
            return 0;
        }

        // 走到这里就是首个节点是成员数, 并且本次要分配多个成员, 检测是否足够分配

        if (_Size > count)
            return 0;   // 回收的链表成员数不够分配, 返回0, 还是看设计, 是遍历链表找分配还是不管

        // 走到这里就是回收的链表成员数足够分配, 分配出去, 然后修改成员数



        if (_Size == count)
        {
            // 节点的成员数等于分配出去的成员数, 那就直接分配出去, 然后链表首节点指向下一个节点
            pHead->pFree = pNextNode;
            if (isClear)
                memset(pArr, 0, sizeof(value_type) * _Size);
            return reinterpret_cast<pointer>(pArr);
        }

        // 走到这里就是分配的成员数小于链表成员数, 需要把链表成员数减去分配的成员数, 然后把分配的成员数返回
        LPBYTE pNodeStart = reinterpret_cast<LPBYTE>(pHead->pFree);
        LPBYTE pNodeEnd = pNodeStart + (sizeof(value_type) * _Size);

        // 回收链表的首节点指向分配出去后的下一个成员地址
        pHead->pFree = reinterpret_cast<PLIST_NODE>(pNodeEnd);

        if (_Size + 1 == count)
        {
            // 分配出去后节点就剩下一个成员, 那就这个成员指向下一个节点
            pHead->pFree->next = pNextNode; // 只剩一个成员了, 那就指向下一个节点
        }
        else
        {
            // 走到这就是剩余1个成员以上, 那就第一个成员记录剩余成员数, 第二个成员指向下一个节点

            pArr = reinterpret_cast<pointer*>(pNodeEnd);
            pArr[0] = reinterpret_cast<pointer>(count - _Size); // 第一个成员记录剩余成员数
            pArr[1] = reinterpret_cast<pointer>(pNextNode);     // 第二个成员指向下一个节点
        }

        if (isClear)
            memset(pNodeStart, 0, sizeof(value_type) * _Size);
        return reinterpret_cast<pointer>(pNodeStart);
    }

    // 分配一块内存, 参数是需要分配多少个成员
    inline PMEMORY_HEAD malloc_head(size_t count)
    {
        // TODO 待定, 每次新分配的内存应该分配多大
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



    // 把释放的地址合并到链表里, 如果是连续的内存, 就合并成一个内存块, 如果不是连续, 就串成链表
    // pHead = 内存块头结构
    // pFreeStart = 释放的内存开始地址
    // pFreeEnd = 释放的内存结束地址
    // _FreeCount = 释放的内存成员数
    inline void combine_free_pointer(PMEMORY_HEAD pHead, LPBYTE pFreeStart, LPBYTE pFreeEnd, int _FreeCount)
    {
        // 分配的这个数组中间有分配出去了数据, 回收到链表里
        // 回收的时候看看第一个节点和当前回收的内存是不是连续的, 回收连续节点/节点连续回收, 连续就当成数组保存
        // 现在回收到链表里有两种模式
        // 1. 第一个不是内存块里的地址, 那就是成员数, 表示这个成员包括后面的成员有多少个是连续的, 最后一个成员指向下一个节点
        // 2. 第一个成员是内存块里的地址, 那就是指向下一个节点

        // 内存块的首尾地址, 判断回收的内存是否和内存块的首尾地址一样, 一样就表示整块内存都被回收了, 还原到初始状态
        LPBYTE pItemStart = reinterpret_cast<LPBYTE>(pHead) + sizeof(MEMORY_HEAD);
        LPBYTE pItemEnd = pItemStart + pHead->size - sizeof(MEMORY_HEAD);
        if (pFreeStart == pItemStart && (pHead->arr == 0 ? pFreeEnd == pItemEnd : pFreeEnd == pHead->arr))
        {
            // 回收的是整块内存, 还原到初始状态
            pHead->arr = pItemStart;
            pHead->pFree = 0;
            return;
        }

        // 首个节点是否是内存块里的地址, 如果不是, 那就表示是这个节点有多少个成员是连续的
        pointer* pArr = reinterpret_cast<pointer*>(pHead->pFree);
        PLIST_NODE pNextNode = 0;
        int count = get_first_node_count(pHead, pNextNode);
        
        // 释放链表中节点是首尾地址, 需要判断回收的内存是不是和这个地址是连续的
        LPBYTE pNodeStart = reinterpret_cast<LPBYTE>(pHead->pFree);
        LPBYTE pNodeEnd = pNodeStart + (sizeof(value_type) * count);

        // 如果 释放的结束地址不等于链表的开始地址, 并且 释放的开始地址不等于链表的结束地址
        // 那就是不连续的内存, 直接把回收的内存放到链表里
        if (pFreeEnd != pNodeStart && pFreeStart != pNodeEnd)
        {
            // 走到这里就是回收的内存像不是连续的, 那就把回收的内存放到链表里
            PLIST_NODE pFirstNode = reinterpret_cast<PLIST_NODE>(pFreeStart);
            if (_FreeCount == 1)
            {
                pFirstNode->next = pHead->pFree;    // 只释放一个成员, 那就放到链表首节点
                pHead->pFree = pFirstNode;          // 首节点指向本次回收的内存
                return;
            }
            // 不连续, 但是本次回收的不止一个成员
            pArr = reinterpret_cast<pointer*>(pFirstNode);
            pArr[0] = reinterpret_cast<pointer>(_FreeCount);        // 第一个成员指向成员数
            pArr[1] = reinterpret_cast<pointer>(pHead->pFree);      // 第二个成员指向下一个节点
            pHead->pFree = pFirstNode;          // 首节点指向本次回收的内存
            return;
        }

        PLIST_NODE pFirstNode = pHead->pFree;   // 首节点
        if (pFreeEnd == pNodeStart)
        {
            // 回收的地址连着首节点, 那首节点就改成回收的地址
            pFirstNode = reinterpret_cast<PLIST_NODE>(pFreeStart);
        }
        else
        {
            // 链表首节点连着回收的内存, 首节点还是链表的首节点, 需要修改成员数

        }


        // 走到这里就是回收的内存和链表里的是连续的
        // 不连续的上面已经处理返回了

        const int newCount = _FreeCount + count;

        pArr = reinterpret_cast<pointer*>(pFirstNode);
        pArr[0] = reinterpret_cast<pointer>(newCount);      // 第一个成员指向成员数
        pArr[1] = reinterpret_cast<pointer>(pNextNode);     // 第二个成员指向下一个节点
        pHead->pFree = pFirstNode;          // 首节点指向本次回收的内存

        // 合并两个节点的地址
        combine_pointer(pHead, pNextNode);

    }

    // 合并首节点和传入节点的地址, 如果两个节点的地址是连续的, 那就把这一块内存合并成一块
    void combine_pointer(PMEMORY_HEAD pHead, PLIST_NODE pNode2)
    {
        if (!pNode2)
            return; // 第二个节点是空节点, 不需要合并

        PLIST_NODE pNext1 = 0, pNext2 = 0, pFirstNode = 0, pNextNode = 0;
        int count1 = GetNodeCount(pHead, pHead->pFree, pNext1);
        int count2 = GetNodeCount(pHead, pNode2, pNext2);

        LPBYTE pNodeStart1 = reinterpret_cast<LPBYTE>(pHead->pFree);
        LPBYTE pNodeEnd1 = pNodeStart1 + (sizeof(value_type) * count1);

        LPBYTE pNodeStart2 = reinterpret_cast<LPBYTE>(pNode2);
        LPBYTE pNodeEnd2 = pNodeStart2 + (sizeof(value_type) * count2);

        if(pNodeStart1 == pNodeEnd2)
        {
            // 节点2的结束地址是节点1的开始地址, 那就是 节点2连着节点1
            // 首节点指向节点2, 下一个节点就是节点2的下一个节点
            // 因为第一个节点指向的就是第二个节点, 所以下一个节点永远的第二个节点指向的下一个节点
            pFirstNode = pNode2;
            pNextNode = pNext2;
        }
        else if (pNodeEnd1 == pNodeStart2)
        {
            // 节点1的结束地址是节点2的开始地址, 那就是 节点1连着节点2
            // 首节点还是节点1
            pFirstNode = pHead->pFree;
            pNextNode = pNext2;
        }
        else
        {
            // 两个节点的地址并不连续, 不需要合并
            return;
        }
        const int newCount = count1 + count2;

        pointer* pArr = reinterpret_cast<pointer*>(pFirstNode);
        pArr[0] = reinterpret_cast<pointer>(newCount);      // 第一个成员指向成员数
        pArr[1] = reinterpret_cast<pointer>(pNextNode);     // 第二个成员指向下一个节点
        pHead->pFree = pFirstNode;          // 首节点指向本次回收的内存

        // 递归合并两个节点的地址
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
    // 获取内存池首个内存块
    PMEMORY_HEAD GetHead() const
    {
        return pool->_Mem;
    }
    // 获取内存池当前操作的内存块
    PMEMORY_HEAD GetNow() const
    {
        return pool->_Now;
    }
    // 获取指定节点占用几个成员, 节点为空则返回0, 不为空最少返回1, 返回大于1表示是多个成员
    int GetNodeCount(PMEMORY_HEAD pHead, PLIST_NODE node, PLIST_NODE& pNextNode)
    {
        return pool->GetNodeCount(pHead, node, pNextNode);
    }
    // 获取起始分配内存的起始地址和结束地址, 返回有多少个成员
    int GetItemStartEnd(PMEMORY_HEAD pHead, LPBYTE& pStart, LPBYTE& pEnd)
    {
        pStart = (LPBYTE)pHead;
        pEnd = pStart + pHead->size;
        pStart = pStart + sizeof(MEMORY_HEAD);
        return (pEnd - pStart) / sizeof(value_type);
    }
    // 传递一个地址, 确认这个地址是内存池里第几个成员, 不是指定成员就触发0xcc断点
    int PointerToIndex(PMEMORY_HEAD pHead, LPCVOID ptr)
    {
        LPBYTE pStart = (LPBYTE)pHead;
        LPBYTE pEnd = pStart + pHead->size;
        LPBYTE pFirst = pStart + sizeof(MEMORY_HEAD);

        const int index = IsItemAddress(ptr, pFirst, pEnd);
        if (index == -1)
            __debugbreak(); // 传入了错误的地址, 断下调试
        return index;
    }

    // 判断传入的地址是否是开始和结束里并且是对齐成员的地址, 返回成员所以, 不是成员地址就返回-1
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

    // 检测传入的地址是否是回收链表里的地址
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
                return true;   // 在回收的链表里

            node = pNextNode;
        }
        return false;    // 遍历了回收链表, 没有在链表里
    }

    // 传递一个地址, 判断这个地址是否是内存池里的地址, 并且被分配出去, 如果传入的不是内存池的地址就触发0xcc断点
    int IsAllocatord(PMEMORY_HEAD pHead, LPCVOID ptr)
    {
        // 调用一次判断这个地址是否合法, 如果不合法, 这个函数会断下
        // 这个类本身就不是为了效率, 而是为了绝对的准确, 所以这里调用一次判断地址是否合法
        PointerToIndex(pHead, ptr);

        // 首地址就是需要分配出去的地址, 结束地址是下一个分配的地址
        // 如果下一个分配的地址为空, 那就是指向这块内存的结束地址
        LPBYTE pStart = reinterpret_cast<LPBYTE>(pHead) + sizeof(MEMORY_HEAD);
        LPBYTE pEnd = pHead->arr ? pHead->arr : (pStart + pHead->size - sizeof(MEMORY_HEAD));
        LPBYTE pFirst = pStart;

        LPBYTE p = (LPBYTE)ptr;
        const int index = IsItemAddress(ptr, pFirst, pEnd);
        if (index == -1 && p >= pFirst && p < pEnd)
        {
            __debugbreak(); // 传入了错误的地址, 断下调试
            return false;
        }

        // 没有在起始分配到当前分配的地址中, 那就是没有被分配出去
        if (index == -1)
            return false;
        
        // 已经从数组里分配出去了, 这里检测是否在回收的链表里, 如果在, 那就是没分配出去
        return IsFreeList(pHead, ptr) == false;
    }

};

NAMESPACE_MEMORYPOOL_END


