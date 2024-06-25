

#include <iostream>
#include <Windows.h>
#include <vector>
#include "CMemoryPool.h"
#include "CMemoryObjectPool.h"

using namespace kuodafu;
#if CMEMORYPOOL_ISDEBUG
CMemoryObjectPool pool_obj;
CMemoryPool pool;
#else
CMemoryObjectPool<int> pool_obj;
CMemoryPool pool;
#endif
int* test()
{
    static int i;
    int* p = pool_obj.malloc();
    *p = ++i;
    return p;
}
int main()
{
    std::vector<int*>* arr_1 = new std::vector<int*>;
    std::vector<int*>* arr_2 = new std::vector<int*>;
    const int alloc_size = 10;
    auto s = GetTickCount64();

    auto asdsdf  = realloc(0, 123);
    pool_obj.init(alloc_size);


    {
        // 申请一个数组, 再申请一个成员打断连续, 然后回收数组, 继续分配
        auto p11 = test();
        test();
        auto p12 = test();
        test();
        auto p13 = test();
        test();
        pool_obj.free(p12);
        pool_obj.free(p11);
        test();

        typedef struct LIST_NODE
        {
            LIST_NODE* next;     // 下一个节点
        }*PLIST_NODE;
        typedef struct MEMORY_HEAD
        {
            MEMORY_HEAD* next;       // 下一个块分配的内存
            size_t          size;       // 这一块内存占用的字节
            PLIST_NODE      pFree;      // 回收回来的内存, 如果都没有回收, 那整个值就是0, 数组也分配完了就需要开辟新的内存块
            LPBYTE          arr;        // 分配出去的内存, 每次分配出去都指向下一个成员, 直到越界后就从链表中取下一个节点
        }*PMEMORY_HEAD;

        int* pArr = pool_obj.malloc_arr(4);
        for (int i = 0; i < 4; i++)
            pArr[i] = i;

        auto p1 = test();
        pool_obj.free_arr(pArr, 4);
        auto p2 = test();
        pool_obj.free(p1);
        pArr = pool_obj.malloc_arr(4);
    }


    std::vector<int*>& arr1 = *arr_1;
    arr1.resize(alloc_size);
    int i = 0, free_index = 0;
    for (int*& p : arr1)
    {
        p = pool_obj.malloc();
        *p = ++i;
        if (rand() % 17 == 0)
        {
            pool_obj.free(arr1[free_index]);
            arr1[free_index++] = nullptr;
        }
    }
    s = GetTickCount64() - s;
    std::cout << "内存池分配" << alloc_size << "个成员耗时:" << s << "ms" << std::endl;

    s = GetTickCount64();
    pool_obj.Release();
    s = GetTickCount64() - s;
    delete arr_1;
    std::cout << "释放内存池分配的内存, " << alloc_size << "个成员耗时:" << s << "ms" << std::endl;


    s = GetTickCount64();

    std::vector<int*>& arr2 = *arr_2;
    arr2.resize(alloc_size);
    i = 0;
    for (int*& p : arr2)
    {
        p = new int;
        *p = ++i;
        if (rand() % 17 == 0)
        {
            delete arr2[free_index];
            arr2[free_index++] = nullptr;
        }
    }
    s = GetTickCount64() - s;

    std::cout << "new分配" << alloc_size << "个成员耗时:" << s << "ms" << std::endl;
    s = GetTickCount64();
    for (int*& p : arr2)
    {
        if (p)
            delete p;
    }
    s = GetTickCount64() - s;

    delete arr_2;
    std::cout << "释放new分配的内存, " << alloc_size << "个成员耗时:" << s << "ms" << std::endl;


    std::allocator<int >a;
    //int* ptr = a.allocate(10);
    //a.deallocate(ptr, 10);

    //int saaa;
    std::cin >> s;

}

