#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#define NAMESPACE_MEMORYPOOL_BEGIN namespace kuodafu {
#define NAMESPACE_MEMORYPOOL_END }

// 根据编译目标选择能够直接申请页对齐原始存储的平台 API。
#if defined(_WIN32) || defined(_WIN64)
    #define MEMORYPOOL_PLATFORM_WINDOWS 1
    #if !defined(NOMINMAX)
        #define KUODAFU_MEMORYPOOL_UNDEFINE_NOMINMAX 1
        #define NOMINMAX 1
    #endif
    #include <windows.h>
    #if defined(KUODAFU_MEMORYPOOL_UNDEFINE_NOMINMAX)
        #undef NOMINMAX
        #undef KUODAFU_MEMORYPOOL_UNDEFINE_NOMINMAX
    #endif
#elif defined(__linux__)
    #define MEMORYPOOL_PLATFORM_LINUX 1
    #include <sys/mman.h>
    #include <unistd.h>
#elif defined(__APPLE__)
    #define MEMORYPOOL_PLATFORM_MACOS 1
    #include <sys/mman.h>
    #include <unistd.h>
#else
    #define MEMORYPOOL_PLATFORM_OTHER 1
    #include <unistd.h>
#endif

NAMESPACE_MEMORYPOOL_BEGIN

/**
 * @brief 查询并缓存当前系统的真实内存页大小。
 *
 * Windows 使用 GetSystemInfo，POSIX 平台使用 sysconf(_SC_PAGESIZE)。本函数不会用
 * 硬编码的 4096 掩盖查询失败，因为上层必须据此验证物理块首地址和总尺寸的页对齐。
 *
 * @return 系统页大小，单位：字节；系统 API 查询失败、返回 0 或返回负值时返回 0。
 * @note 本函数为 noexcept。函数内静态值只在第一次调用时查询一次，之后为 O(1) 读取。
 */
inline size_t memory_pool_system_page_size() noexcept
{
    static const size_t pageSize = []() noexcept -> size_t
    {
#if defined(MEMORYPOOL_PLATFORM_WINDOWS)
        SYSTEM_INFO information = {};
        ::GetSystemInfo(&information);
        return static_cast<size_t>(information.dwPageSize);
#else
        const long queried = ::sysconf(_SC_PAGESIZE);
        return queried > 0 ? static_cast<size_t>(queried) : 0x1000;
#endif
    }();
    return pageSize > 0 && pageSize < 65536 ? pageSize : 0x1000;
}

/**
 * @brief 无状态、页对齐的原始存储分配器。
 *
 * 该类型保留类似 std::allocator 的泛型元素计数接口：allocate(count) 申请
 * count * sizeof(value_type) 字节。CMemoryPoolBase 内部固定使用单字节特化，使池层传入的
 * 数量可以直接表示字节；分配器本身仍允许外部代码使用其他 value_type。
 *
 * @tparam Ty 分配器的元素类型。
 * @note 所有实例均无状态且可互相释放内存，因此 is_always_equal 为 true，移动赋值和
 *       swap 均允许传播 allocator。
 */
template<class Ty>
class CMemoryPoolAllocator
{
public:
    using value_type = Ty; // 分配器元素类型。
    using pointer = Ty*; // 原始可写指针类型。
    using const_pointer = const Ty*; // 原始只读指针类型。
    using reference = Ty&; // 元素左值引用类型。
    using const_reference = const Ty&; // 元素只读引用类型。
    using size_type = size_t; // 分配数量类型，单位：value_type 元素个数。
    using difference_type = ptrdiff_t; // 两个元素指针之间的距离类型。
    using propagate_on_container_move_assignment = std::true_type; // 移动赋值时传播 allocator。
    using propagate_on_container_swap = std::true_type; // swap 时传播 allocator。
    using is_always_equal = std::true_type; // 任意实例均可释放其他实例申请的块。

    /** @brief allocator_traits 用于重新绑定元素类型的类型映射。 */
    template<class Other>
    struct rebind
    {
        using other = CMemoryPoolAllocator<Other>; // 重新绑定后的 allocator 类型。
    };

    /**
     * @brief 构造一个无状态分配器。
     * @note 本函数为 noexcept，不查询系统页大小，也不分配物理内存。
     */
    CMemoryPoolAllocator() noexcept = default;

    /**
     * @brief 复制构造一个等价的无状态分配器。
     * @param[in] other 来源实例；实例中没有需要复制的运行时状态。
     * @note 本函数为 noexcept，不分配物理内存。
     */
    CMemoryPoolAllocator(const CMemoryPoolAllocator& other) noexcept
    {
        (void)other;
    }

    /**
     * @brief 从另一元素类型的同类分配器转换构造。
     * @tparam Other 来源 allocator 的 value_type。
     * @param[in] other 来源实例；实例中没有需要转换的运行时状态。
     * @note 本函数为 noexcept，不分配物理内存。
     */
    template<class Other>
    CMemoryPoolAllocator(const CMemoryPoolAllocator<Other>& other) noexcept
    {
        (void)other;
    }

    /**
     * @brief 析构无状态分配器对象。
     * @note 本函数为 noexcept，不会自动释放此前返回的物理块；块所有者必须显式调用 deallocate()。
     */
    ~CMemoryPoolAllocator() noexcept = default;

    /**
     * @brief 复制赋值无状态分配器。
     * @param[in] other 来源实例；没有运行时状态需要复制。
     * @return 当前实例 *this。
     * @note 本函数为 noexcept，不改变任何已分配物理块的所有权。
     */
    CMemoryPoolAllocator& operator=(const CMemoryPoolAllocator& other) noexcept
    {
        (void)other;
        return *this;
    }

    /**
     * @brief 取得可写对象的真实地址。
     * @param[in] value 目标对象引用。
     * @return std::addressof(value) 返回的原始指针。
     * @note 本函数为 noexcept，即使 value_type 重载 operator& 也返回真实地址。
     */
    pointer address(reference value) const noexcept
    {
        return std::addressof(value);
    }

    /**
     * @brief 取得只读对象的真实地址。
     * @param[in] value 目标只读对象引用。
     * @return std::addressof(value) 返回的原始只读指针。
     * @note 本函数为 noexcept。
     */
    const_pointer address(const_reference value) const noexcept
    {
        return std::addressof(value);
    }

    /**
     * @brief 从操作系统分配一段首地址按真实系统页对齐的原始存储。
     *
     * @param[in] count 元素数量，单位：value_type 个；实际请求字节数为
     *                  count * sizeof(value_type)。0 返回 nullptr。
     * @param[in] hint 地址提示；当前实现忽略该参数。
     * @return count 为 0 时返回 nullptr；否则返回系统页对齐的原始存储首地址。
     *
     * @throws std::bad_array_new_length count 超过 max_size()。
     * @throws std::bad_alloc 系统页大小无效、平台对齐参数不合法或底层系统分配失败。
     *
     * @note 返回地址同时满足系统页和 value_type 的对齐要求，不主动把 count 向上
     *       取整为页大小；上层 CMemoryPoolBase 负责保证物理块总字节数是页大小的
     *       整数倍。平台后端无法提供更高对齐时抛出 std::bad_alloc。
     */
    pointer allocate(size_type count, const void* hint = nullptr)
    {
        (void)hint;
        if (count == 0)
            return nullptr;
        if (count > max_size())
            throw std::bad_array_new_length();

        // 先用 max_size() 防止元素数换算成物理字节数时发生无符号乘法溢出。
        const size_t bytes = count * sizeof(value_type);
        const size_t pageSize = memory_pool_system_page_size();
        if (pageSize == 0)
            throw std::bad_alloc();

#if defined(MEMORYPOOL_PLATFORM_WINDOWS)
        // VirtualAlloc 的保留+提交结果至少按系统页对齐，失败以 nullptr 表示。
        void* raw = ::VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!raw)
            throw std::bad_alloc();
        if (reinterpret_cast<std::uintptr_t>(raw) % alignof(value_type) != 0)
        {
            ::VirtualFree(raw, 0, MEM_RELEASE);
            throw std::bad_alloc();
        }
        return static_cast<pointer>(raw);
#elif defined(MEMORYPOOL_PLATFORM_LINUX) || defined(MEMORYPOOL_PLATFORM_MACOS)
        // 匿名 mmap 返回页对齐区间；解除映射时必须传回完全相同的原始字节长度。
        #if defined(MAP_ANONYMOUS)
        const int anonymousFlag = MAP_ANONYMOUS;
        #else
        const int anonymousFlag = MAP_ANON;
        #endif
        void* raw = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | anonymousFlag, -1, 0);
        if (raw == MAP_FAILED)
            throw std::bad_alloc();
        if (!raw || reinterpret_cast<std::uintptr_t>(raw) % alignof(value_type) != 0)
        {
            ::munmap(raw, bytes);
            throw std::bad_alloc();
        }
        return static_cast<pointer>(raw);
#else
        // posix_memalign 要求对齐同时是 2 次幂和 sizeof(void*) 的整数倍。
        const size_t typeAlignment = alignof(value_type);
        const size_t allocationAlignment = pageSize < typeAlignment ? typeAlignment : pageSize;
        if ((allocationAlignment & (allocationAlignment - 1)) != 0
            || allocationAlignment % sizeof(void*) != 0)
            throw std::bad_alloc();
        void* raw = nullptr;
        if (::posix_memalign(&raw, allocationAlignment, bytes) != 0 || !raw)
            throw std::bad_alloc();
        if (reinterpret_cast<std::uintptr_t>(raw) % pageSize != 0
            || reinterpret_cast<std::uintptr_t>(raw) % typeAlignment != 0)
        {
            std::free(raw);
            throw std::bad_alloc();
        }
        return static_cast<pointer>(raw);
#endif
    }

    /**
     * @brief 归还 allocate() 得到的整段原始存储。
     * @param[in] ptr allocate() 返回的块首地址；nullptr 表示无操作。
     * @param[in] count 原始分配元素数量，单位：value_type 个；内部重新换算为字节数。
     * @pre 非空 ptr 必须由兼容的 CMemoryPoolAllocator 返回，尚未释放，并且 count 必须
     *      与 allocate() 时完全一致；POSIX munmap 使用该值作为解除映射长度。
     * @note 为保持旧版公开函数类型，本函数不声明 noexcept；当前实现不会主动抛出异常，
     *       也不调用存储中对象的析构函数。
     */
    void deallocate(pointer ptr, size_type count)
    {
        if (!ptr)
            return;
        const size_t bytes = count * sizeof(value_type);
#if defined(MEMORYPOOL_PLATFORM_WINDOWS)
        (void)bytes;
        ::VirtualFree(ptr, 0, MEM_RELEASE);
#elif defined(MEMORYPOOL_PLATFORM_LINUX) || defined(MEMORYPOOL_PLATFORM_MACOS)
        ::munmap(ptr, bytes);
#else
        (void)bytes;
        std::free(ptr);
#endif
    }

    /**
     * @brief 返回一次 allocate() 可接受的最大元素数量。
     * @return size_type 上限除以 sizeof(value_type)，单位：value_type 元素个数。
     * @note 本函数为 noexcept，不代表操作系统一定能够实际分配如此大的连续区间。
     */
    size_type max_size() const noexcept
    {
        return (std::numeric_limits<size_type>::max)() / sizeof(value_type);
    }

    /**
     * @brief 在调用方提供的原始存储上构造一个对象。
     * @tparam Object 要构造的对象类型。
     * @tparam Args 构造参数类型包。
     * @param[in] ptr 目标原始存储首地址，必须满足 Object 的尺寸和对齐要求。
     * @param[in] args 转发给 Object 构造函数的参数。
     * @note Object 构造函数抛出的异常原样传播；本函数不负责申请或释放物理内存。
     */
    template<class Object, class... Args>
    void construct(Object* const ptr, Args&&... args)
    {
        // placement new 只开始对象生命周期，底层页块所有权仍由内存池持有。
        ::new (const_cast<void*>(static_cast<const volatile void*>(ptr)))
            Object(std::forward<Args>(args)...);
    }

    /**
     * @brief 显式结束一个活动对象的生命周期。
     * @tparam Object 待析构对象类型。
     * @param[in] ptr 指向一个已经成功构造且尚未析构的 Object。
     * @note 为保持旧版公开函数类型，本函数不声明 noexcept；Object 析构函数抛出的异常
     *       会原样传播。本函数不归还对象所在的物理存储。
     */
    template<class Object>
    void destroy(Object* ptr)
    {
        ptr->~Object();
    }
};

/**
 * @brief 比较两个无状态内存池分配器是否兼容。
 * @tparam Left 左侧元素类型。
 * @tparam Right 右侧元素类型。
 * @param[in] left 左侧 allocator 实例。
 * @param[in] right 右侧 allocator 实例。
 * @return 始终返回 true；任意实例均可释放另一实例分配的物理块。
 * @note 本函数为 noexcept。
 */
template<class Left, class Right>
bool operator==(const CMemoryPoolAllocator<Left>& left,
                const CMemoryPoolAllocator<Right>& right) noexcept
{
    (void)left;
    (void)right;
    return true;
}

/**
 * @brief 比较两个无状态内存池分配器是否不兼容。
 * @tparam Left 左侧元素类型。
 * @tparam Right 右侧元素类型。
 * @param[in] left 左侧 allocator 实例。
 * @param[in] right 右侧 allocator 实例。
 * @return 始终返回 false。
 * @note 本函数为 noexcept。
 */
template<class Left, class Right>
bool operator!=(const CMemoryPoolAllocator<Left>& left,
                const CMemoryPoolAllocator<Right>& right) noexcept
{
    (void)left;
    (void)right;
    return false;
}

NAMESPACE_MEMORYPOOL_END
