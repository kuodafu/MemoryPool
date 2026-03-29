
#include <iostream>
#include <iomanip>
#include <vector>
#include <random>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <windows.h>
#include <psapi.h>
#include <mimalloc.h>
#include "CMemoryObjectPool.h"

using namespace kuodafu;

using alloc_type = int;

// 写入测试数据: 简单类型用随机值赋值, 结构体用 memset 随机字节
inline void write_test_data(alloc_type* p, unsigned int seed)
{
    seed = 0x66;
    std::memset(p, seed & 0xFF, (std::min)(sizeof(alloc_type), sizeof(seed)));
}

// 每个分配器需要提供 alloc 和 free 两个函数
// 通过模板特化或重载实现不同分配器

// ==================== 分配器定义 ====================

// 1. 内存池分配器
struct Allocator_MemoryPool
{
    //CMemoryObjectPool<alloc_type, std::allocator<uint8_t>> pool;
    CMemoryObjectPool<alloc_type, CMemoryPoolAllocator<uint8_t>> pool;
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
    alloc_type* alloc()
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

// 7. mimalloc
struct Allocator_mimalloc
{
    alloc_type* alloc() { return static_cast<alloc_type*>(mi_malloc(sizeof(alloc_type))); }
    void free(alloc_type* p) { mi_free(p); }
    const char* name() const { return "mimalloc"; }
};

// ==================== 测试框架 ====================

struct StressTestResult
{
    double alloc_time;     // 分配耗时
    double random_rw_time; // 随机读写耗时
    double random_free_time; // 随机释放耗时
    double realloc_time;   // 再分配耗时
    double random_rw2_time; // 再次随机读写耗时
    double free_all_time;  // 释放所有耗时
    double total_time;     // 总耗时
    double mem_before_alloc;
    double mem_before_free;
    double mem_after_free;
    int count;

    StressTestResult() : alloc_time(0), random_rw_time(0), random_free_time(0),
        realloc_time(0), random_rw2_time(0), free_all_time(0), total_time(0),
        mem_before_alloc(0), mem_before_free(0), mem_after_free(0), count(0) {}

    StressTestResult& operator+=(const StressTestResult& other)
    {
        alloc_time += other.alloc_time;
        random_rw_time += other.random_rw_time;
        random_free_time += other.random_free_time;
        realloc_time += other.realloc_time;
        random_rw2_time += other.random_rw2_time;
        free_all_time += other.free_all_time;
        total_time += other.total_time;
        mem_before_alloc += other.mem_before_alloc;
        mem_before_free += other.mem_before_free;
        mem_after_free += other.mem_after_free;
        count += other.count;
        return *this;
    }
};

// 打印测试结果
inline void print_result(const char* name, const StressTestResult& acc)
{
    double c = static_cast<double>(acc.count);
    double a_alloc = acc.alloc_time / c;
    double a_rw = acc.random_rw_time / c;
    double a_rfree = acc.random_free_time / c;
    double a_realloc = acc.realloc_time / c;
    double a_rw2 = acc.random_rw2_time / c;
    double a_free = acc.free_all_time / c;
    double a_total = acc.total_time / c;
    double a_bef_alloc = acc.mem_before_alloc / c;
    double a_bef_free = acc.mem_before_free / c;
    double a_aft_free = acc.mem_after_free / c;

    std::cout << "| " << std::left << std::setw(12) << name << " | "
              << std::right << std::setw(8) << a_alloc << " | "
              << std::setw(8) << a_rw << " | "
              << std::setw(8) << a_rfree << " | "
              << std::setw(8) << a_realloc << " | "
              << std::setw(9) << a_rw2 << " | "
              << std::setw(8) << a_free << " | "
              << std::setw(8) << a_total << " | "
              << std::setw(7) << a_bef_alloc << " | "
              << std::setw(7) << a_bef_free << " | "
              << std::setw(7) << a_aft_free << " |" << std::endl;
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

    StressTestResult r;
    r.mem_before_alloc = get_process_memory_mb();

    // 1. 分配 _Count 次
    auto t0 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < _Count; i++)
    {
        alloc_type* p = alloc.alloc();
        write_test_data(p, val_dist(rng));
        live_ptrs.push_back(p);
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    // 2. 随机读写
    std::uniform_int_distribution<size_t> idx_dist(0, live_ptrs.size() - 1);
    for (size_t i = 0; i < _Count; i++)
    {
        size_t idx = idx_dist(rng);
        alloc_type* p = live_ptrs[idx];
        write_test_data(p, val_dist(rng));
        volatile alloc_type x = *p; (void)x;
    }
    auto t2 = std::chrono::high_resolution_clock::now();

    // 3. 随机释放一半
    size_t half = live_ptrs.size() / 2;
    for (size_t i = 0; i < half; i++)
    {
        std::uniform_int_distribution<size_t> dist(0, live_ptrs.size() - 1);
        size_t idx = dist(rng);
        alloc.free(live_ptrs[idx]);
        live_ptrs[idx] = live_ptrs.back();
        live_ptrs.pop_back();
    }
    auto t3 = std::chrono::high_resolution_clock::now();

    r.mem_before_free = get_process_memory_mb();

    // 4. 再分配填补空缺
    for (size_t i = 0; i < half; i++)
    {
        alloc_type* p = alloc.alloc();
        write_test_data(p, val_dist(rng));
        live_ptrs.push_back(p);
    }
    auto t4 = std::chrono::high_resolution_clock::now();

    // 5. 再次随机读写
    for (size_t i = 0; i < _Count; i++)
    {
        size_t idx = idx_dist(rng);
        alloc_type* p = live_ptrs[idx];
        write_test_data(p, val_dist(rng));
        volatile alloc_type x = *p; (void)x;
    }
    auto t5 = std::chrono::high_resolution_clock::now();

    // 6. 释放所有
    for (alloc_type* p : live_ptrs)
        alloc.free(p);
    auto t6 = std::chrono::high_resolution_clock::now();

    r.mem_after_free = get_process_memory_mb();

    r.alloc_time     = std::chrono::duration<double, std::milli>(t1 - t0).count();
    r.random_rw_time = std::chrono::duration<double, std::milli>(t2 - t1).count();
    r.random_free_time = std::chrono::duration<double, std::milli>(t3 - t2).count();
    r.realloc_time   = std::chrono::duration<double, std::milli>(t4 - t3).count();
    r.random_rw2_time = std::chrono::duration<double, std::milli>(t5 - t4).count();
    r.free_all_time  = std::chrono::duration<double, std::milli>(t6 - t5).count();
    r.total_time     = std::chrono::duration<double, std::milli>(t6 - t0).count();
    r.count = 1;
    return r;
}

// ==================== 内存池压力测试 ====================

struct PoolStressResult
{
    double total_time;
    double alloc_time;
    double free_time;
    double mem_peak;
    size_t count;

    PoolStressResult() : total_time(0), alloc_time(0), free_time(0), mem_peak(0), count(0) {}

    PoolStressResult& operator+=(const PoolStressResult& other)
    {
        total_time += other.total_time;
        alloc_time += other.alloc_time;
        free_time += other.free_time;
        mem_peak += other.mem_peak;
        count += other.count;
        return *this;
    }
};

// 场景1: 每次分配后立即释放, 测试 alloc+free 的配对效率
template<class _Alloc>
PoolStressResult test_alloc_free_pair(_Alloc& alloc, size_t _Count, int _Seed)
{
    std::mt19937 rng(static_cast<unsigned int>(_Seed));
    std::uniform_int_distribution<int> val_dist(0, 0x7FFFFFFF);

    PoolStressResult r;
    double peak = 0;

    auto t0 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < _Count; i++)
    {
        alloc_type* p = alloc.alloc();
        write_test_data(p, val_dist(rng));
        double mem = get_process_memory_mb();
        if (mem > peak) peak = mem;
        alloc.free(p);
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    r.total_time = std::chrono::duration<double, std::milli>(t1 - t0).count();
    r.alloc_time = r.total_time * 0.5;  // 近似
    r.free_time  = r.total_time * 0.5;
    r.mem_peak   = peak;
    r.count = 1;
    return r;
}

// 场景2: 批量分配后批量释放, 测试批量操作的吞吐
template<class _Alloc>
PoolStressResult test_batch_alloc_free(_Alloc& alloc, size_t _Count, int _Seed)
{
    std::mt19937 rng(static_cast<unsigned int>(_Seed));
    std::uniform_int_distribution<int> val_dist(0, 0x7FFFFFFF);

    std::vector<alloc_type*> ptrs;
    ptrs.reserve(_Count);

    PoolStressResult r;
    double peak = 0;

    auto t0 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < _Count; i++)
    {
        alloc_type* p = alloc.alloc();
        write_test_data(p, val_dist(rng));
        ptrs.push_back(p);
        double mem = get_process_memory_mb();
        if (mem > peak) peak = mem;
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < _Count; i++)
        alloc.free(ptrs[i]);
    auto t2 = std::chrono::high_resolution_clock::now();

    r.alloc_time  = std::chrono::duration<double, std::milli>(t1 - t0).count();
    r.free_time  = std::chrono::duration<double, std::milli>(t2 - t1).count();
    r.total_time = r.alloc_time + r.free_time;
    r.mem_peak   = peak;
    r.count = 1;
    return r;
}

// 场景3: 固定数量池子, 持续随机分配/释放, 模拟对象池
template<class _Alloc>
PoolStressResult test_fixed_pool(_Alloc& alloc, size_t _PoolSize, size_t _Ops, int _Seed)
{
    std::mt19937 rng(static_cast<unsigned int>(_Seed));
    std::uniform_int_distribution<int> val_dist(0, 0x7FFFFFFF);
    std::uniform_int_distribution<int> op_dist(0, 1); // 0=分配, 1=释放

    std::vector<alloc_type*> live;
    live.reserve(_PoolSize);

    PoolStressResult r;
    double peak = 0;

    // 预热: 先把池子填满一半
    for (size_t i = 0; i < _PoolSize / 2; i++)
    {
        alloc_type* p = alloc.alloc();
        write_test_data(p, val_dist(rng));
        live.push_back(p);
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < _Ops; i++)
    {
        if (op_dist(rng) == 0 || live.empty())
        {
            // 分配
            alloc_type* p = alloc.alloc();
            write_test_data(p, val_dist(rng));
            live.push_back(p);
        }
        else
        {
            // 随机释放一个
            std::uniform_int_distribution<size_t> idx_dist(0, live.size() - 1);
            size_t idx = idx_dist(rng);
            alloc.free(live[idx]);
            live[idx] = live.back();
            live.pop_back();
        }
        double mem = get_process_memory_mb();
        if (mem > peak) peak = mem;
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    // 清理
    for (alloc_type* p : live)
        alloc.free(p);

    r.total_time = std::chrono::duration<double, std::milli>(t1 - t0).count();
    r.mem_peak   = peak;
    r.count = 1;
    return r;
}

// 场景4: 高复用模式 - 分配, 写, 释放, 再分配同一指针槽位
template<class _Alloc>
PoolStressResult test_high_reuse(_Alloc& alloc, size_t _Count, int _Seed)
{
    std::mt19937 rng(static_cast<unsigned int>(_Seed));
    std::uniform_int_distribution<int> val_dist(0, 0x7FFFFFFF);

    PoolStressResult r;
    double peak = 0;

    auto t0 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < _Count; i++)
    {
        alloc_type* p = alloc.alloc();
        write_test_data(p, val_dist(rng));
        volatile alloc_type x = *p; (void)x;
        double mem = get_process_memory_mb();
        if (mem > peak) peak = mem;
        alloc.free(p);
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    r.total_time = std::chrono::duration<double, std::milli>(t1 - t0).count();
    r.alloc_time = r.total_time * 0.5;
    r.free_time  = r.total_time * 0.5;
    r.mem_peak   = peak;
    r.count = 1;
    return r;
}

inline void print_pool_result(const char* scenario, const char* name, const PoolStressResult& r)
{
    double c = static_cast<double>(r.count);
    std::cout << "| " << std::left << std::setw(17) << scenario
              << " | " << std::setw(12) << name
              << " | " << std::right << std::setw(10) << std::fixed << std::setprecision(2) << r.total_time / c
              << " | " << std::setw(10) << r.alloc_time / c
              << " | " << std::setw(10) << r.free_time / c
              << " | " << std::setw(10) << r.mem_peak / c << " |" << std::endl;
}

template<class _Alloc>
void run_pool_stress(_Alloc& alloc, const char* name, size_t N, int baseSeed)
{
    constexpr int WARMUP = 2;
    constexpr int ROUND  = 10;

    PoolStressResult acc;

    // 预热
    for (int i = 0; i < WARMUP; i++)
    {
        PoolStressResult w;
        w += test_alloc_free_pair(alloc, N, baseSeed + i);
        w += test_batch_alloc_free(alloc, N, baseSeed + i);
        w += test_fixed_pool(alloc, N / 10, N, baseSeed + i);
        w += test_high_reuse(alloc, N, baseSeed + i);
    }

    // 正式测试
    for (int i = 0; i < ROUND; i++)
    {
        acc += test_alloc_free_pair(alloc, N, baseSeed + i);
    }
    print_pool_result("配对 alloc+free", name, acc);
    acc = PoolStressResult();

    for (int i = 0; i < ROUND; i++)
    {
        acc += test_batch_alloc_free(alloc, N, baseSeed + i);
    }
    print_pool_result("批量 batch", name, acc);
    acc = PoolStressResult();

    for (int i = 0; i < ROUND; i++)
    {
        acc += test_fixed_pool(alloc, N / 10, N, baseSeed + i);
    }
    print_pool_result("固定池 随机", name, acc);
    acc = PoolStressResult();

    for (int i = 0; i < ROUND; i++)
    {
        acc += test_high_reuse(alloc, N, baseSeed + i);
    }
    print_pool_result("高复用 reuse", name, acc);
    std::cout << "|-------------------|--------------|------------|------------|------------|------------|" << std::endl;
}

// ==================== 主测试 ====================

int main()
{
    constexpr size_t N = 1000000;
    constexpr int SEED = 12345;
    constexpr int ROUND = 10;

    std::cout << "========== 内存分配器暴力测试 ==========" << std::endl;
    std::cout << "分配次数: " << N << " 次/轮, 测试 " << ROUND << " 轮取平均" << std::endl;
    std::cout << std::endl;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "| Allocator    |   分配   | 随机读写 | 随机释放 |  再分配  | 随机读写2 | 释放所有 |   总计   | 分配前  | 释放前  | 释放后  |" << std::endl;

    {
        // 预热两轮
        Allocator_MemoryPool a;
        for (int i = 0; i < 2; i++)
            stress_test(a, N, SEED + i);
    }

    std::cout << "|--------------|----------|----------|----------|----------|-----------|----------|----------|---------|---------|---------|" << std::endl;

    // --- MemoryPool ---
    {
        Allocator_MemoryPool a;
        a.pool.init(0x1000000);
        StressTestResult acc;
        for (int i = 0; i < ROUND; i++)
            acc += stress_test(a, N, SEED + i);
        print_result("MemoryPool", acc);
    }

    //// --- HeapAlloc ---
    //{
    //    Allocator_HeapAlloc a;
    //    StressTestResult acc;
    //    for (int i = 0; i < ROUND; i++)
    //        acc += stress_test(a, N, SEED + i);
    //    print_result("HeapAlloc", acc);
    //}

    //// --- malloc ---
    //{
    //    Allocator_malloc a;
    //    StressTestResult acc;
    //    for (int i = 0; i < ROUND; i++)
    //        acc += stress_test(a, N, SEED + i);
    //    print_result("malloc", acc);
    //}

    //// --- new ---
    //{
    //    Allocator_new a;
    //    StressTestResult acc;
    //    for (int i = 0; i < ROUND; i++)
    //        acc += stress_test(a, N, SEED + i);
    //    print_result("new", acc);
    //}

    //// --- new[] ---
    //{
    //    Allocator_newArray a;
    //    StressTestResult acc;
    //    for (int i = 0; i < ROUND; i++)
    //        acc += stress_test(a, N, SEED + i);
    //    print_result("new[]", acc);
    //}

    //// --- mimalloc ---
    //{
    //    Allocator_mimalloc a;
    //    StressTestResult acc;
    //    for (int i = 0; i < ROUND; i++)
    //        acc += stress_test(a, N, SEED + i);
    //    print_result("mimalloc", acc);
    //}

    std::cout << "|--------------------------------------------------------------------------------------------------------------------------|" << std::endl;

    std::cout << "\n========== 内存池固定尺寸持续分配/释放压力测试 ==========" << std::endl;
    std::cout << "分配次数: " << N << " 次, 测试 " << ROUND << " 轮取平均, 预热 " << 2 << " 轮" << std::endl;
    std::cout << std::endl;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "|        场景       |    Allocator |     总耗时 |   分配耗时 |   释放耗时 |   峰值内存 |" << std::endl;
    std::cout << "|-------------------|--------------|------------|------------|------------|------------|" << std::endl;

    // --- MemoryPool ---
    {
        Allocator_MemoryPool a;
        run_pool_stress(a, "MemoryPool", N, SEED);
    }

    // --- HeapAlloc ---
    {
        Allocator_HeapAlloc a;
        run_pool_stress(a, "HeapAlloc", N, SEED);
    }

    // --- malloc ---
    {
        Allocator_malloc a;
        run_pool_stress(a, "malloc", N, SEED);
    }

    // --- new ---
    {
        Allocator_new a;
        run_pool_stress(a, "new", N, SEED);
    }

    // --- mimalloc ---
    {
        Allocator_mimalloc a;
        run_pool_stress(a, "mimalloc", N, SEED);
    }

    std::cout << "\n测试完成, 按回车退出..." << std::endl;
    std::cin.get();
}
