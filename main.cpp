
#include <iostream>
#include <iomanip>
#include <vector>
#include <random>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <windows.h>
#include <psapi.h>
#include "CMemoryObjectPool.h"

using namespace kuodafu;

using alloc_type = int;

// 每个分配器需要提供 alloc 和 free 两个函数
// 通过模板特化或重载实现不同分配器

// ==================== 分配器定义 ====================

// 1. 内存池分配器
struct Allocator_MemoryPool
{
    CMemoryObjectPool<alloc_type> pool;
    alloc_type* alloc() { return pool.malloc(); }
    void free(alloc_type* p) { pool.free(p); }
    const char* name() const { return "MemoryPool"; }
};

// 2. malloc / free
struct Allocator_malloc
{
    alloc_type* alloc() { return static_cast<alloc_type*>(std::malloc(sizeof(alloc_type))); }
    void free(alloc_type* p) { std::free(p); }
    const char* name() const { return "malloc"; }
};

// 3. HeapAlloc / HeapFree
struct Allocator_HeapAlloc
{
    HANDLE hHeap;
    Allocator_HeapAlloc() { hHeap = HeapCreate(0, 0, 0); }
    ~Allocator_HeapAlloc() { HeapDestroy(hHeap); }
    alloc_type* alloc() { return static_cast<alloc_type*>(HeapAlloc(hHeap, 0, sizeof(alloc_type))); }
    void free(alloc_type* p) { HeapFree(hHeap, 0, p); }
    const char* name() const { return "HeapAlloc"; }
};

// 4. VirtualAlloc / VirtualFree
struct Allocator_VirtualAlloc
{
    int* alloc()
    {
        return static_cast<alloc_type*>(VirtualAlloc(nullptr, sizeof(alloc_type), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    }
    void free(alloc_type* p) { VirtualFree(p, 0, MEM_RELEASE); }
    const char* name() const { return "VirtualAlloc"; }
};

// 5. new / delete
struct Allocator_new
{
    using Ty = alloc_type;
    alloc_type* alloc() { return new alloc_type; }
    void free(alloc_type* p) { delete p; }
    const char* name() const { return "new"; }
};

// 6. new[] / delete[]
struct Allocator_newArray
{
    using Ty = alloc_type;
    alloc_type* alloc() { return new alloc_type[1]; }
    void free(alloc_type* p) { delete[] p; }
    const char* name() const { return "new[]"; }
};

// ==================== 测试框架 ====================

struct StressTestResult
{
    double alloc_time;   // 分配耗时 (ms)
    double free_time;    // 释放耗时 (ms)
    double total_time;   // 总耗时 (ms)
    double mem_before_alloc;  // 分配前内存 (MB)
    double mem_before_free;   // 释放前内存 (MB)
    double mem_after_free;    // 释放后内存 (MB)
    int count;           // 累计次数

    StressTestResult() : alloc_time(0), free_time(0), total_time(0),
        mem_before_alloc(0), mem_before_free(0), mem_after_free(0), count(0) {}

    StressTestResult& operator+=(const StressTestResult& other)
    {
        alloc_time += other.alloc_time;
        free_time += other.free_time;
        total_time += other.total_time;
        mem_before_alloc += other.mem_before_alloc;
        mem_before_free += other.mem_before_free;
        mem_after_free += other.mem_after_free;
        count += other.count;
        return *this;
    }
};

// 打印测试结果: 接收名称、累加结果、基准时间，自动对齐
inline void print_result(const char* name, const StressTestResult& acc, double baseline_time)
{
    double avg_alloc = acc.alloc_time / acc.count;
    double avg_free = acc.free_time / acc.count;
    double avg_total = acc.total_time / acc.count;
    double avg_bef_alloc = acc.mem_before_alloc / acc.count;
    double avg_bef_free = acc.mem_before_free / acc.count;
    double avg_aft_free = acc.mem_after_free / acc.count;

    std::cout << "| " << std::left << std::setw(16) << name << " | "
              << std::right << std::setw(8) << avg_alloc << " | "
              << std::setw(8) << avg_free << " | "
              << std::setw(8) << avg_total << " | "
              << std::setw(10) << avg_bef_alloc << " | "
              << std::setw(10) << avg_bef_free << " | "
              << std::setw(10) << avg_aft_free << " | "
              << std::setw(4) << std::fixed << std::setprecision(2) << (avg_total / baseline_time) << " |" << std::endl;
}

// 获取进程内存占用 (MB)
inline double get_process_memory_mb()
{
    PROCESS_MEMORY_COUNTERS_EX2 pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
        return pmc.PrivateWorkingSetSize / (1024.0 * 1024.0);
    return 0.0;
}

// 暴力测试: 随机分配 / 随机释放 / 随机读写
// _Alloc: 分配器类型
// _Count: 每次测试分配多少轮
// _Seed: 随机种子, 保证不同分配器用相同序列的随机操作
template<class _Alloc>
StressTestResult stress_test(_Alloc& alloc, size_t _Count, int _Seed)
{
    std::mt19937 rng(static_cast<unsigned int>(_Seed));
    std::uniform_int_distribution<int> val_dist(0, 0x7FFFFFFF);

    std::vector<alloc_type*> live_ptrs;
    live_ptrs.reserve(_Count);

    double mem_before_alloc = get_process_memory_mb();

    // 分配阶段: 分配 _Count 次
    auto t0 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < _Count; i++)
    {
        alloc_type* p = alloc.alloc();
        *p = static_cast<alloc_type>(val_dist(rng));
        live_ptrs.push_back(p);
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    // 随机读写阶段: 随机选一个存活的指针读写
    std::uniform_int_distribution<size_t> idx_dist(0, live_ptrs.size() - 1);
    for (size_t i = 0; i < _Count; i++)
    {
        size_t idx = idx_dist(rng);
        alloc_type* p = live_ptrs[idx];
        *p = static_cast<alloc_type>(val_dist(rng));
        volatile alloc_type x = *p; (void)x;  // 防止编译器优化掉读
    }

    // 随机释放阶段: 随机释放一半的存活指针
    size_t half = live_ptrs.size() / 2;
    for (size_t i = 0; i < half; i++)
    {
        std::uniform_int_distribution<size_t> dist(0, live_ptrs.size() - 1);
        size_t idx = dist(rng);
        alloc.free(live_ptrs[idx]);
        live_ptrs[idx] = live_ptrs.back();
        live_ptrs.pop_back();
    }

    double mem_before_free = get_process_memory_mb();

    // 再分配: 填补被释放的空缺
    for (size_t i = 0; i < half; i++)
    {
        alloc_type* p = alloc.alloc();
        *p = static_cast<alloc_type>(val_dist(rng));
        live_ptrs.push_back(p);
    }

    // 再次随机读写
    for (size_t i = 0; i < _Count; i++)
    {
        size_t idx = idx_dist(rng);
        alloc_type* p = live_ptrs[idx];
        *p = static_cast<alloc_type>(val_dist(rng));
        volatile alloc_type x = *p; (void)x;
    }

    // 释放所有
    auto t2 = std::chrono::high_resolution_clock::now();
    for (alloc_type* p : live_ptrs)
        alloc.free(p);
    auto t3 = std::chrono::high_resolution_clock::now();

    double mem_after_free = get_process_memory_mb();

    StressTestResult r;
    r.alloc_time      = std::chrono::duration<double, std::milli>(t1 - t0).count();
    r.free_time       = std::chrono::duration<double, std::milli>(t3 - t2).count();
    r.total_time      = std::chrono::duration<double, std::milli>(t3 - t0).count();
    r.mem_before_alloc = mem_before_alloc;
    r.mem_before_free  = mem_before_free;
    r.mem_after_free   = mem_after_free;
    r.count = 1;
    return r;
}

// ==================== 主测试 ====================

int main()
{
    constexpr size_t N = 1000000;   // 每次测试分配 100 万次
    constexpr int SEED = 12345;     // 固定随机种子
    constexpr int ROUND = 1;      // 每种分配器跑 10 轮取平均

    std::cout << "========== 内存分配器暴力测试 ==========" << std::endl;
    std::cout << "分配次数: " << N << " 次/轮, 测试 " << ROUND << " 轮取平均" << std::endl;
    std::cout << std::endl;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "| Allocator        | 分配(ms) | 释放(ms) | 总计(ms) | 分配前(M)  | 释放前(M)  | 释放后(M)  | 相对 |" << std::endl;
    std::cout << "|------------------|----------|----------|----------|------------|------------|------------|------|" << std::endl;

    double baseline_time = 0;

    // --- MemoryPool ---
    {
        Allocator_MemoryPool a;
        StressTestResult acc;
        for (int i = 0; i < ROUND; i++)
            acc += stress_test(a, N, SEED + i);
        baseline_time = acc.total_time / acc.count;
        print_result("MemoryPool", acc, baseline_time);
    }

    // --- malloc ---
    {
        Allocator_malloc a;
        StressTestResult acc;
        for (int i = 0; i < ROUND; i++)
            acc += stress_test(a, N, SEED + i);
        print_result("malloc", acc, baseline_time);
    }

    // --- HeapAlloc ---
    {
        Allocator_HeapAlloc a;
        StressTestResult acc;
        for (int i = 0; i < ROUND; i++)
            acc += stress_test(a, N, SEED + i);
        print_result("HeapAlloc", acc, baseline_time);
    }

    // --- VirtualAlloc ---
    {
        Allocator_VirtualAlloc a;
        StressTestResult acc;
        for (int i = 0; i < 1; i++)
            acc += stress_test(a, N, SEED + i);
        print_result("VirtualAlloc", acc, baseline_time);
    }

    // --- new ---
    {
        Allocator_new a;
        StressTestResult acc;
        for (int i = 0; i < ROUND; i++)
            acc += stress_test(a, N, SEED + i);
        print_result("new", acc, baseline_time);
    }

    // --- new[] ---
    {
        Allocator_newArray a;
        StressTestResult acc;
        for (int i = 0; i < ROUND; i++)
            acc += stress_test(a, N, SEED + i);
        print_result("new[]", acc, baseline_time);
    }

    std::cout << "|--------------------------------------------------------------------------------------------------|\n\n";
 
    std::cout << "测试完成, 按回车退出..." << std::endl;
    std::cin.get();
}
