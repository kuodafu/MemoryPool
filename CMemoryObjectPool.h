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
    PLIST_NODE      list;       // 回收回来的内存, 如果都没有回收, 那这个值就是0, 数组也分配完了就需要开辟新的内存块
    LPBYTE          item;       // 分配出去的内存, 每次分配出去都指向下一个成员, 直到越界后就从链表中取下一个节点
}*PMEMORY_HEAD;


#if CMEMORYPOOL_ISDEBUG
class CMemoryPoolView;
#else
template<class _Ty = LPVOID, class _Alloc = std::allocator<BYTE>> class CMemoryPoolView;
#endif

// 定长内存池, 每次分配都是固定大小的内存
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


    _Alloc              _Al;        // 分配器, 分配内存都是按字节分配, 自己计算分配成员需要多少字节 + 头部结构
    PMEMORY_HEAD        _Mem;       // 内存块头结构, 分配和回收都根据这个结构来操作
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
            _Al.deallocate(reinterpret_cast<LPBYTE>(node), node->size);
            node = next;
        }
        _Mem = nullptr;
        _Now = nullptr;
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
            pHead->item = reinterpret_cast<LPBYTE>(pHead) + sizeof(MEMORY_HEAD);
            pHead->list = nullptr;   // 没有回收的内存

            pHead = pHead->next;
        }
    }

    // 返回当前内存池总共占用的字节数, 包括已经分配和没分配的
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
    // 初始化内存池, 内部会申请空间
    inline bool init(size_t size = 0x1000)
    {
        if (_Mem)
            return true;

        _Mem = malloc_head(size);
        _Now = _Mem;        // 当前操作的内存块
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
    inline pointer malloc(bool isClear = false)
    {
        return malloc_arr(1, isClear);
    }


    // 申请一个数组, 释放内存请使用 free_arr() 释放, 否则只会释放第一个成员
    // 申请失败则抛出 std::bad_alloc 类型异常
    // _Size = 申请多少个成员
    inline pointer malloc_arr(size_t _Size, bool isClear = false)
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
            // 先从回收链表里分配, 如果分配失败就从所有内存块里分配
            // 先消耗链表, 不要让链表深度太深, 否则会导致链表遍历时间过长
            if (pHead->list)
                pRet = alloc_for_list(pHead, _Size, isClear, 0);

            // 再从item里分配
            if (!pRet && pHead->item)
                pRet = alloc_for_item(pHead, _Size, isClear);
            
            return pRet != nullptr;
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
        PMEMORY_HEAD pHead = get_head(pArr);
        if (!pHead)
        {
            throw std::exception(__FUNCTION__ ": 传递进来了不是内存池里的地址", 1);
            return 0;
        }

        pointer pRet = 0;
        LPBYTE pEnd = reinterpret_cast<LPBYTE>(pHead) + pHead->size;
        LPBYTE pArrStart = reinterpret_cast<LPBYTE>(pArr);
        LPBYTE pArrEnd = pArrStart + sizeof(value_type) * _OldSize;
        bool isFree = true;
        if (pArrEnd == pHead->item || pArrEnd == pEnd)
        {
            // 被释放的这个数组是连着下一个分配的内存, 直接把下一个分配的内存指向回收的这个数组
            pHead->item = pArrStart;
            isFree = false;     // pHead->arr = pArrStart; 这一行已经释放了, 把变量设置为假, 后面不需要继续释放

        }

        // 先尝试从回收链表里分配, 如果分配失败就从所有内存块里分配
        // 先消耗链表, 不要让链表深度太深, 否则会导致链表遍历时间过长
        pRet = alloc_for_list(pHead, _Size, false, 0);
        if(pRet && !isFree)
            return pRet;    // 如果这一个内存块分配成功, 并且isFree为false, 那就是在原来数组的地址上分配成功了
        
        if (!pRet && pHead->list)
            pRet = alloc_for_item(pHead, _Size, false);    // 从链表分配失败后就尝试从item里分配

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
        PMEMORY_HEAD pHead = get_head(p);
        if (!pHead)
        {
            throw std::exception(__FUNCTION__ ": 传递进来了不是内存池里的地址", 1);
            return false;
        }

        LPBYTE pArrStart = reinterpret_cast<LPBYTE>(p);
        LPBYTE pArrEnd = pArrStart + sizeof(value_type) * _Size;
        LPBYTE pEnd = reinterpret_cast<LPBYTE>(pHead) + pHead->size;
        if (pArrEnd == pHead->item || pArrEnd == pEnd)
        {
            // 被释放的这个数组是连着下一个分配的内存, 直接把下一个分配的内存指向回收的这个数组
            pHead->item = pArrStart;
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
    inline bool query(pointer p) const
    {
        return get_head(p) != nullptr;
    }
    // 输出当前内存池的状态, 输出每个内存块的状态
    inline void dump() const
    {
        PMEMORY_HEAD pHead = _Mem;
        int index = 0;
        while (pHead)
        {
            LPBYTE pAllocStart = reinterpret_cast<LPBYTE>(pHead) + sizeof(MEMORY_HEAD);
            int count = static_cast<int>(((pHead->item - pAllocStart) / sizeof(value_type)));
            printf("%03d: 内存块首地址 0x%p, 内存块起始分配地址: 0x%p, 内存块尺寸: %u, 已分配 %d 个成员, 当前回收的内存链表 = 0x%p\n",
                index++, pHead, pAllocStart, (UINT)pHead->size, count, pHead->list);
            pHead = pHead->next;
        }
    }

private:
    // 检测这个地址是否是这个内存块的地址
    inline bool is_head(PMEMORY_HEAD pHead, LPCVOID p) const
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
    // 从内存块的数组里分配成员出去
    inline pointer alloc_for_item(PMEMORY_HEAD pHead, size_t _Size, bool isClear) const
    {
        LPBYTE pEnd = reinterpret_cast<LPBYTE>(pHead) + pHead->size;
        LPBYTE ptr = pHead->item;
        const size_t offset = _Size * sizeof(value_type);
        if (ptr + offset > pEnd)
            return 0;   // 内存不够分配一个数组, 返回0

        pHead->item += offset;    // 指向下一个分配的地址
        if (pHead->item >= pEnd)
        {
            pHead->item = 0;  // 已经分配到尾部了, 重置为0
        }

        if (isClear)
            memset(ptr, 0, offset);
        return reinterpret_cast<pointer>(ptr);
    }

    // 获取指定节点占用几个成员, 节点为空则返回0, 不为空最少返回1, 返回大于1表示是多个成员
    size_t node_get_count(PMEMORY_HEAD pHead, PLIST_NODE node, PLIST_NODE& pNextNode) const
    {
        pNextNode = nullptr;
        if (!node)
            return 0;

        size_t count = 1;  // 走到这里表示回收链表里有节点, 那返回的成员数最少为1

        // 回收链表首节点的下一个节点不为0, 那就表示next要么存着成员数, 要么存着下一个节点地址
        if (node->next != nullptr)
        {
            // next不为0, 那就是要么存成员数, 要么存下一个节点指针
            // pArr[0] == pHead->pFree->next, 这两个是一样的
            pointer* pArr = reinterpret_cast<pointer*>(node);
            if (is_head(pHead, node->next))
            {
                pNextNode = node->next; // next存放的是下一个节点地址
            }
            else
            {
                // next存放的是成员数
                count = reinterpret_cast<size_t>(pArr[0]);
                pNextNode = reinterpret_cast<PLIST_NODE>(pArr[1]);
            }
        }
        return count;
    }

    struct NODE_MIN_COUNT
    {
        size_t count;       // 返回的节点占用的成员数
        PLIST_NODE node;    // 返回的节点
        PLIST_NODE next;    // 返回节点的下一个节点
        PLIST_NODE prev;    // 返回节点的上一个节点
    };

    // 从内存块的链表节点里分配成员出去, 能走到这个方法的话, 回收的链表肯定不为空
    // 先遍历回收链表, 看看哪个节点刚好够分出去, 取成员最小最接近的节点分配出去
    inline pointer alloc_for_list(PMEMORY_HEAD pHead, size_t _Size, bool isClear, size_t stack)
    {

        NODE_MIN_COUNT min_node{};
        if (!list_get_min_node(pHead, _Size, min_node))
            return nullptr; // 整个链表没有节点足够分配

        // node 就是要分配出去的内存, count 是node 占用的成员数
        if (min_node.count == _Size)
        {
            // 要返回的节点刚好足够分配出去, 直接返回
            node_set_next(pHead, min_node.prev, min_node.next);
            return reinterpret_cast<pointer>(min_node.node);
        }

        // 走到这就是节点可用成员大于要分配出去的成员
        // 调整当前节点, 然后分配出去

        LPBYTE pNodeStart = reinterpret_cast<LPBYTE>(min_node.node);
        LPBYTE pNodeEnd = pNodeStart + (sizeof(value_type) * _Size);
        auto _List = reinterpret_cast<PLIST_NODE>(pNodeEnd);

        size_t new_count = min_node.count - _Size;

        // 分配出去后当前节点就只剩下一个成员
        if (new_count == 1)
        {
            // 分配出去后节点就剩下一个成员, 那就这个成员指向下一个节点
            _List->next = min_node.next;    // 只剩一个成员了, 那就指向下一个节点
        }
        else
        {
            // 走到这就是剩余1个成员以上, 那就第一个成员记录剩余成员数, 第二个成员指向下一个节点
            auto pArr = reinterpret_cast<pointer*>(_List);

            pArr[0] = reinterpret_cast<pointer>(new_count);     // 第一个成员记录剩余成员数
            pArr[1] = reinterpret_cast<pointer>(min_node.next); // 第二个成员指向下一个节点
        }

        // 让上一个节点指向 _List
        node_set_next(pHead, min_node.prev, _List);

        if (isClear)
            memset(pNodeStart, 0, sizeof(value_type) * _Size);
        return reinterpret_cast<pointer>(pNodeStart);
    }

    // 获取节点里和要分配的成员数最接近的节点, 并返回节点的成员数
    // pHead = 内存块
    // _Size = 要分配的成员数
    // ret_node = 接收返回的内容
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

            // 如果当前节点够分配, 那就先记录起来
            if (node_count >= _Size)
            {
                // 当前节点够分配, 把最小的成员记录起来, 要是循环结束都没有找到合适的节点, 那就返回这个节点
                if (ret_node.count == 0 || node_count < ret_node.count)
                {
                    // 先把这个节点保存起来, 如果有下一个更合适的会被覆盖
                    ret_node.node = node;
                    ret_node.count = node_count;
                    ret_node.next = next;
                    ret_node.prev = prev;
                }
            }

            // 当前节点和需要分配的成员刚好一样, 不需要往下找了, 直接返回
            if (node_count == _Size)
                break;

            // 继续下一轮循环, 节点不够分配的情况不需要处理, 直接下一轮循环就可以

            prev = node;    // 记录上一个节点, 第一次循环的时候上一个节点为空
            node = next;    // 指向下一个节点, 继续循环
        }

        // 只要 ret_node.node 有赋过值, 那就表示肯定有合适的节点返回
        return ret_node.node != nullptr;
    }

    // 获取链表里和传递进来的地址相连的节点, 首尾/ 尾首 都算相连
    // pHead = 内存块
    // pStart = 要判断的起始地址
    // pEnd = 要判断的结束地址
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

    // 判断两个首尾地址是否是相连的, 1连着2 返回1, 2连着1 返回-1, 不相连返回0
    int is_memory_adjacent(LPBYTE pStart1, LPBYTE pEnd1, LPBYTE pStart2, LPBYTE pEnd2)
    {
        // 如果第一个尾地址和第二个首地址一样, 就表示 1 连着 2
        if (pEnd1 == pStart2)
            return 1;
        
        // 如果第二个尾地址和第一个首地址一样, 就表示 2 连着 1
        if (pEnd2 == pStart1)
            return -1;

        return 0;
    }
    // 分配一块内存, 参数是需要分配多少个成员
    inline PMEMORY_HEAD malloc_head(size_t count)
    {
        // TODO 待定, 每次新分配的内存应该分配多大
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


    // TODO 把释放的地址放到链表里, 链表是有序的, 每次加入都得遍历加入位置, 如果能合并到节点就合并
    // 这个释放会影响速度, 但是这个只能存放一个指针的空间, 除了链表不知道有什么数据结构能用
    // pHead = 内存块头结构
    // pFreeStart = 释放的内存开始地址
    // pFreeEnd = 释放的内存结束地址
    // _FreeCount = 释放的内存成员数
    inline bool combine_free_pointer(PMEMORY_HEAD pHead, LPBYTE pFreeStart, LPBYTE pFreeEnd, size_t _FreeCount)
    {
        // 分配的这个数组中间有分配出去了数据, 回收到链表里
        // 回收的时候看看节点和当前回收的内存是不是连续的, 回收连续节点/节点连续回收, 连续就当成数组保存
        // 现在回收到链表里有两种模式
        // 1. 第一个不是内存块里的地址, 那就是成员数, 表示这个成员包括后面的成员有多少个是连续的, 最后一个成员指向下一个节点
        // 2. 第一个成员是内存块里的地址, 那就是指向下一个节点

        // 内存块的首尾地址, 判断回收的内存是否和内存块的首尾地址一样, 一样就表示整块内存都被回收了, 还原到初始状态
        LPBYTE pItemStart = reinterpret_cast<LPBYTE>(pHead) + sizeof(MEMORY_HEAD);
        LPBYTE pItemEnd = pItemStart + pHead->size - sizeof(MEMORY_HEAD);
        if (pFreeStart == pItemStart && (pHead->item == 0 ? pFreeEnd == pItemEnd : pFreeEnd == pHead->item))
        {
            // 回收的是整块内存, 还原到初始状态
            pHead->item = pItemStart;
            pHead->list = nullptr;
            return true;
        }

        //////////////////////////////////////////////////////////////////////////
        // 遍历链表, 找到加入的位置, 加入链表是从小到大排序的, 所以找到第一个比当前节点大的节点, 就插入到当前节点的后面
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
                // 回收的内存 连着 当前节点, 回收的内存在当前节点的左边, 调整当前节点的起始地址
                if (adjacent > 0)
                    pNodeStart = pFreeStart;
                else
                    pNodeEnd = pFreeEnd;

                size_t newCount = (pNodeEnd - pNodeStart) / sizeof(value_type);
                // 回收的内存和当前节点可用合并, 合并后更改一下节点数量, 然后就可以返回了
                auto pArr = reinterpret_cast<pointer*>(pNodeStart);
                pArr[0] = reinterpret_cast<pointer>(newCount);      // 第一个成员指向成员数
                pArr[1] = reinterpret_cast<pointer>(next);          // 第二个成员指向下一个节点
                return true;
            }


            // 回收的内存 和 当前节点 不连着, 判断是否是加入当前位置
            if (pFreeStart > pNodeEnd)
            {
                // 回收的地址在当前节点的右边, 加入到节点的后面, 然后返回
                PLIST_NODE free_node = node_make(pFreeStart, pFreeStart);
                node_set_next(pHead, node, free_node);  // 当前节点的下一个节点就是回收发内存
                node_set_next(pHead, free_node, next);  // 回收的内存的下一个节点就是当前节点的下一个节点
                return true;
            }
            
            // 继续往下走, 如果走出循环, 那就是要加入到链表尾部
            prev = node;
            node = next;
        }

        PLIST_NODE free_node = node_make(pFreeStart, pFreeStart);
        node_set_next(pHead, prev, free_node);  // 直接加入到链表尾部, 走出循环, prev就是最后一个节点
        return true;
    }

    // 在一段内存之间生成一个节点
    PLIST_NODE node_make(LPBYTE pStart, LPBYTE pEnd)
    {
        size_t count = (pEnd - pStart) / sizeof(value_type);
        PLIST_NODE pNode = reinterpret_cast<PLIST_NODE>(pStart);
        node_set_value(pNode, count, nullptr);
        return pNode;
    }

    // 合并链表节点, 遍历整个链表, 把首节点和剩下的所有节点比较, 看看有没有相连的内存
    void merge_nodes(PMEMORY_HEAD pHead)
    {
        return; // 废案
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
                // 第一个节点连着 当前枚举的节点, pEnd 改成 pNodeEnd
                pEnd = pNodeEnd;
            }
            else if (adjacent < 0)
            {
                // 当前枚举的节点 连着 第一个节点
                pStart = pNodeStart;
            }
            if (adjacent)
            {
                // 指针已经合并了, 这里合并节点

                if (node == first_next)
                {
                    // 当前节点是第一个节点的下一个节点, 跟第一个节点合并了
                    // 这里得把第一个节点的下一个节点更新一下
                    first_next = next;
                }
                else
                {
                    // 不是第一个节点的下一个节点, 合并到当前节点
                    // 先把当前节点断开链表, 让上一个节点指向下一个节点
                    node_set_next(pHead, prev, next);
                }

                // 把当前节点断开链表, 让上一个节点指向下一个节点
                node_set_next(pHead, prev, next);

            }

            prev = node;
            node = next;
            _Count++;
        }

        size_t new_count = (pEnd - pStart) / sizeof(value_type);
        node_set_value(first_node, new_count, nullptr);
    }

    // 遍历链表, 每遍历一个都调用回调函数去处理
    // pHead = 内存块头结构
    // _Pred = 回调函数(接收节点, 下一个节点, 上一个节点, 节点成员数, 节点序号), 返回0继续遍历, 返回非0终止遍历
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

    // 递归合并传入的两个节点, 如果两个节点的地址是连续的, 那就把这一块内存合并成一块
    void combine_node(PMEMORY_HEAD pHead, PLIST_NODE pNode1, PLIST_NODE pNode2)
    {
        if (!pNode2 || !pNode1)
            return; // 是空节点, 不需要合并

        PLIST_NODE pNext1 = nullptr, pNext2 = nullptr, pFirstNode = nullptr, pNextNode = nullptr;
        const size_t count1 = node_get_count(pHead, pNode1, pNext1);
        const size_t count2 = node_get_count(pHead, pNode2, pNext2);

        LPBYTE pNodeStart1 = reinterpret_cast<LPBYTE>(pNode1);
        LPBYTE pNodeEnd1 = pNodeStart1 + (sizeof(value_type) * count1);

        LPBYTE pNodeStart2 = reinterpret_cast<LPBYTE>(pNode2);
        LPBYTE pNodeEnd2 = pNodeStart2 + (sizeof(value_type) * count2);
        pNextNode = pNext2;

        // 先判断节点2的结尾地址是否和节点1的起始地址相连, 相连就说明两个节点是连续的
        if(pNodeStart1 == pNodeEnd2)
        {
            // 节点2的结束地址是节点1的开始地址, 那就是 节点1连着节点2
            // 首节点指向节点2, 下一个节点就是节点2的下一个节点
            // 因为第一个节点指向的就是第二个节点, 所以下一个节点永远的第二个节点指向的下一个节点
            pFirstNode = pNode2;
        }
        // 再判断节点1的结尾地址是否和节点2的起始地址相连
        else if (pNodeEnd1 == pNodeStart2)
        {
            // 节点1的结束地址是节点2的开始地址, 那就是 节点2连着节点1
            // 首节点还是节点1
            pFirstNode = pHead->list;
        }
        // 如果上面两个判断都不成立, 那就表示这两个节点不是连续的
        else
        {
            // 两个节点的地址并不连续, 不需要合并
            return;
        }
        const size_t newCount = count1 + count2;

        node_set_value(pFirstNode, newCount, pNextNode);

        pHead->list = pFirstNode;          // 首节点指向本次回收的内存

        // 递归合并两个节点的地址
        combine_node(pHead, pFirstNode, pNextNode);

    }

    // 设置节点的下一个节点 pPrevNode->next = pNextNode, 会判断是按数组保存还是按下一个节点保存
    // 如果上一个节点为空指针, 那就把下一个节点赋值给首节点
    inline void node_set_next(PMEMORY_HEAD pHead, PLIST_NODE pPrevNode, PLIST_NODE pNextNode)
    {
        if (!pPrevNode)
        {
            pHead->list = pNextNode;
            return;
        }
        // 需要判断上一个节点是存放一个指针还是多个成员
        PLIST_NODE next = nullptr;
        size_t count = node_get_count(pHead, pPrevNode, next);
        node_set_value(pPrevNode, count, pNextNode);

    }

    // 设置节点信息
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

    // 计算这个内存块的中间地址, 回收链表有两个, 一个是记录中间往左的地址, 一个是记录中间往右的地址
    // 这样回收内存的时候可以快一倍, 先取中间地址, 然后判断是放到左边还是右边
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

    // 获取待分配内存, 不包含回收链表里的内存, 只记录所有内存块里连续地址那剩余的可分配内存
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
    // 获取回收的内存总尺寸, 返回单位为字节
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
        return (int)pool->node_get_count(pHead, node, pNextNode);
    }
    // 获取起始分配内存的起始地址和结束地址, 返回有多少个成员
    int GetItemStartEnd(PMEMORY_HEAD pHead, LPBYTE& pStart, LPBYTE& pEnd)
    {
        pStart = (LPBYTE)pHead;
        pEnd = pStart + pHead->size;
        pStart = pStart + sizeof(MEMORY_HEAD);
        return (int)((pEnd - pStart) / sizeof(value_type));
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
        LPBYTE p = (LPBYTE)ptr;
        if (p < begin || p >= end)
            return -1;

        const size_t offset = p - begin;
        if (offset % sizeof(value_type) != 0)
            return -1;

        return (int)(offset / sizeof(value_type));
    };

    // 检测传入的地址是否是回收链表里的地址
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
                return true;   // 在回收的链表里

            node = pNextNode;
        }
        return false;    // 遍历了回收链表, 没有在链表里
    }

    // 传递一个地址, 判断这个地址是否是内存池里的地址, 并且被分配出去, 如果传入的不是内存池的地址就触发0xcc断点
    int IsAllocated(PMEMORY_HEAD pHead, LPCVOID ptr)
    {
        // 调用一次判断这个地址是否合法, 如果不合法, 这个函数会断下
        // 这个类本身就不是为了效率, 而是为了绝对的准确, 所以这里调用一次判断地址是否合法
        PointerToIndex(pHead, ptr);

        // 首地址就是需要分配出去的地址, 结束地址是下一个分配的地址
        // 如果下一个分配的地址为空, 那就是指向这块内存的结束地址
        LPBYTE pStart = reinterpret_cast<LPBYTE>(pHead) + sizeof(MEMORY_HEAD);
        LPBYTE pEnd = pHead->item ? pHead->item : (pStart + pHead->size - sizeof(MEMORY_HEAD));
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


