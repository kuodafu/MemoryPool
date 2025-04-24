

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
CMemoryObjectPool<size_t> pool_obj;
CMemoryPool pool;
#endif

// 测试随机删除插入, 先初始化一段数据, 然后在这个基础上随机删除插入, 看看内存池的表现
void test_random_delete_insert();

int main()
{
    std::vector<int*>* arr_1 = new std::vector<int*>;
    std::vector<int*>* arr_2 = new std::vector<int*>;
    const int alloc_size = 10000;
    auto s = GetTickCount64();

    auto asdsdf  = realloc(0, 123);
    pool_obj.init(alloc_size);

    test_random_delete_insert();
    return 0;
}

void test_random_delete_insert()
{
    //UINT num_srand = (UINT)time(nullptr);
    UINT num_srand = (UINT)1745373544;
    srand(num_srand);
    CMemoryPoolView pool_view(&pool_obj);
    //CMemoryPoolView<size_t> pool_view(&pool_obj);
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    COORD coord = { 0, 0 };

    std::vector<size_t*> arr;
    arr.reserve(10000);

    HANDLE hHeap = HeapCreate(0, 0, 0);
    HeapSetInformation(hHeap, HeapEnableTerminationOnCorruption, nullptr, 0);


    // 记录总共分配了多少字节的内存
    int alloc_size = 0;
    auto pfn_alloc = [&](size_t size) -> size_t*
    {
        //size_t* ptr = pool_obj.malloc_arr(size, true);
        size_t* ptr = (size_t*)HeapAlloc(hHeap, HEAP_ZERO_MEMORY, size * sizeof(size_t));
        *ptr = size;
        alloc_size += (int)(size * sizeof(size_t));
        return ptr;
    };
    auto pfn_free = [&](size_t* ptr)
    {
        if (!ptr)
            return;

        size_t size = *ptr;
        //pool_obj.free_arr(ptr, size);
        HeapFree(hHeap, 0, ptr);
        alloc_size -= (int)(size * sizeof(size_t));
    };



    auto pfn_rand = [](int min, int max)
    {
        return rand() % (max - min + 1) + min;
    };
    int count = pfn_rand(3000, 8000);
    for (int i = 0; i < count; i++)
    {
        size_t size = pfn_rand(3, 20);
        size_t* ptr = pfn_alloc(size);
        arr.push_back(ptr);
    }


    while (true)
    {
        if (rand() % 2 == 1)
        {
            // 分配内存, 每次分配不固定, 看看释放和分配的效果
            size_t size = pfn_rand(3, 20);
            size_t* ptr = pfn_alloc(size);
            for (size_t i = 1; i < size; i++)
            {
                int sub_size = pfn_rand(1, 5);  // 每次分配大小不一样
                size_t* sub_ptr = pfn_alloc(sub_size);
                ptr[i] = (size_t)sub_ptr;
            }
            int insert_index = pfn_rand(0, (int)arr.size());
            arr.insert(arr.begin() + insert_index, ptr);    // 随机插入到数组里
        }
        else
        {
            // 释放内存, 随机从数组获取一个成员释放
            int insert_index = pfn_rand(0, (int)arr.size() - 1);
            size_t* ptr = arr[insert_index];
            if (ptr)
            {
                size_t size = (size_t)*ptr;
                for (size_t i = 1; i < size; i++)
                    pfn_free((size_t*)ptr[i]);
                pfn_free(ptr);
            }
            arr.erase(arr.begin() + insert_index);
        }


        SetConsoleCursorPosition(hConsole, coord); // 移动光标到第一行第一列
        int listCount = 0;
        double mb = (double)pool_obj.size() / 1024.0 / 1024.0;
        double list_mb = (double)pool_view.GetFreeListSize(listCount) / 1024 / 1024;
        double item_mb = (double)pool_view.GetItemSize() / 1024 / 1024;
        double alloc_mb = (double)alloc_size / 1024 / 1024;
        static ULONG64 _i;
        static ULONG64 prev_time;
        _i++;

        if (GetTickCount64() - prev_time > 500)
        {
            prev_time = GetTickCount64();
            printf("%llu, 随机数种子: %u\n"
                   "当前项目数\t%d\t原始项目数\t%d\n"
                   "内存池总尺寸\t%.2fMB\n"
                   "剩余待分配内存\t%.2fMB\t已占用内存\t%.2fMB\n"
                   "链表总尺寸\t%.2fMB, 链表总节点数\t%d\n"
                   "                     \n",
                   _i, num_srand,
                   (int)arr.size(), count,
                   mb,
                   item_mb, alloc_mb,
                   list_mb, listCount
            );
        }

    }
    //Sleep(1);
}