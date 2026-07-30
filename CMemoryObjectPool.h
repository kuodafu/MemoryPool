#pragma once

#include "CMemoryPoolBase.h"

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

NAMESPACE_MEMORYPOOL_BEGIN

/**
 * @brief 固定类型对象池。
 *
 * 每个 malloc() 成功返回的槽位都已经完成 value_type 构造并提交活动位；free、clear、
 * destroy 和析构函数会且只会析构当前仍活动的对象。构造失败会按照槽位来源精确回滚，
 * 不会把半构造对象暴露给 query/free。
 *
 * @tparam Ty 池中对象类型，其析构函数必须满足 noexcept。
 * @tparam Allocator 分配页对齐物理块的内部单字节 allocator 类型。
 * @note 单个池实例不支持并发访问；调用方必须在池外串行化。
 */
template<class Ty = uint8_t, class Allocator = CMemoryPoolAllocator<uint8_t>>
class CMemoryObjectPool : public CMemoryPoolBase<Allocator>
{
private:
    using Base = CMemoryPoolBase<Allocator>; // 公共块管理实现。

public:
    using value_type = Ty; // 每个活动槽位中构造的对象类型。
    using pointer = value_type*; // 活动对象可写指针类型。
    using const_pointer = const value_type*; // 活动对象只读指针类型。
    using allocator_type = Allocator; // 页对齐物理块 allocator 类型。

protected:
    using typename Base::BytePointer; // 派生析构逻辑使用的块内字节指针类型。
    using typename Base::MemoryHead; // 派生析构 hook 使用的物理块头类型。

public:
    static_assert(std::is_nothrow_destructible<value_type>::value,
                  "CMemoryObjectPool 要求对象析构函数为 noexcept");

    /**
     * @brief 构造使用默认 allocator 的延迟初始化空对象池。
     * @post 不建立物理块；第一次 malloc() 时才按默认预算建立首块。
     * @throws std::length_error sizeof(value_type) 对齐后的最终槽尺寸超过 UINT32_MAX。
     * @note 自定义 Allocator 默认构造若抛异常，该异常原样传播；本构造函数不分配页块。
     */
    CMemoryObjectPool()
        : Base(sizeof(value_type), alignof(value_type), 0)
    {
    }

    /**
     * @brief 构造使用默认 allocator 的对象池，并可立即建立首块。
     * @param[in] count 首块至少容纳的对象槽位数量，单位：个；0 表示延迟初始化。
     * @throws std::length_error 尺寸运算溢出或物理块超过 allocator 上限。
     * @throws std::runtime_error 无法取得有效系统页大小。
     * @throws std::bad_alloc 物理块分配失败或 allocator 页对齐契约不成立。
     * @note 自定义 Allocator 默认构造或 allocate 的其他异常原样传播。
     */
    explicit CMemoryObjectPool(size_t count)
        : Base(sizeof(value_type), alignof(value_type), count)
    {
    }

    /**
     * @brief 复制指定 allocator，构造延迟初始化的空对象池。
     * @param[in] allocator 用于复制构造内部 allocator 的实例。
     * @post 不建立物理块；第一次 malloc() 时才分配。
     * @throws std::length_error sizeof(value_type) 对齐后的最终槽尺寸超过 UINT32_MAX。
     * @note 自定义 Allocator 复制构造若抛异常，该异常原样传播。
     */
    explicit CMemoryObjectPool(const allocator_type& allocator)
        : Base(sizeof(value_type), alignof(value_type), 0, allocator)
    {
    }

    /**
     * @brief 复制指定 allocator 构造对象池，并可立即建立首块。
     * @param[in] count 首块至少容纳的对象槽位数量，单位：个；0 表示延迟初始化。
     * @param[in] allocator 用于复制构造内部 allocator 的实例。
     * @throws std::length_error 尺寸运算溢出或物理块超过 allocator 上限。
     * @throws std::runtime_error 无法取得有效系统页大小。
     * @throws std::bad_alloc 物理块分配失败或页对齐契约不成立。
     * @note 自定义 Allocator 复制构造或 allocate 的其他异常原样传播。
     */
    CMemoryObjectPool(size_t count, const allocator_type& allocator)
        : Base(sizeof(value_type), alignof(value_type), count, allocator)
    {
    }

    /** @brief 禁止复制构造，确保物理块链始终只有一个所有者。 */
    CMemoryObjectPool(const CMemoryObjectPool&) = delete;

    /**
     * @brief 移动接管来源对象池的 allocator、块链、布局和热点状态。
     * @param[in,out] other 来源池；成功后为空，始终可析构或重新赋值。默认 allocator 下
     *                      可直接重新使用；自定义 allocator 取决于其移动后状态契约。
     * @pre other 必须处于 idle；违反该前置条件会按 Base 的 public noexcept 兼容移动规则
     *      触发断言或终止，而不是抛出可恢复异常。
     * @note Allocator 移动构造在 Base 中被编译期约束为 noexcept；本操作为 O(1)。
     */
    CMemoryObjectPool(CMemoryObjectPool&& other)
        : Base(std::move(other))
    {
    }

    /**
     * @brief 析构全部活动 value_type，并归还所有物理块。
     * @note 本函数为 noexcept。必须在派生析构阶段主动调用 Base::destroy()，否则进入
     *       Base 析构后虚派发已不能到达本类的对象析构 hook。
     */
    ~CMemoryObjectPool() noexcept override
    {
        Base::destroy();
    }

    /** @brief 禁止复制赋值，避免对象和物理块产生共享所有权。 */
    CMemoryObjectPool& operator=(const CMemoryObjectPool&) = delete;

    /**
     * @brief 销毁当前池内容后移动接管来源池。
     * @param[in,out] other 来源池；成功后为空且可析构或重新赋值。默认 allocator 下可
     *                      直接复用；自定义 allocator 取决于其移动后状态。自移动无操作。
     * @return 当前对象 *this。
     * @throws std::logic_error 任一池正处于受保护操作。
     * @throws std::invalid_argument allocator 不传播且两个实例不兼容。
     * @note 有状态 allocator 比较的其他异常会在目标旧内容销毁前原样传播；提交阶段不抛。
     */
    CMemoryObjectPool& operator=(CMemoryObjectPool&& other)
    {
        Base::moveAssignPool(std::move(other));
        return *this;
    }

    /**
     * @brief 预留槽位、构造一个 value_type，并在构造成功后提交活动位。
     *
     * 按 value_type 对本参数包的构造属性在编译期三路分派：
     * - 平凡且 nothrow 的构造：不运行任何用户代码，走与字节池相同的"预留即提交"
     *   免事务快路径；
     * - 不抛异常的非平凡构造：保留 constructing 重入保护，但裁掉 try/catch 与回滚；
     * - 可能抛异常的构造（含 CWG1778 允许的"平凡但显式 noexcept(false)"边角类型）：
     *   完整的预留/提交/精确回滚事务，行为与旧版一致。
     *
     * @tparam Args 传递给 value_type 构造函数的参数类型包。
     * @param[in] args 完美转发给 value_type 构造函数的参数。
     * @return 已完成构造的活动对象指针，地址满足 alignof(value_type)。
     * @throws std::logic_error 同池重入或内部 free-list 计数损坏。
     * @throws std::length_error 扩块尺寸运算溢出或超过 allocator 上限。
     * @throws std::runtime_error 无法取得有效系统页大小。
     * @throws std::bad_alloc 新物理块分配失败或页对齐契约不成立。
     * @note 所有分派路径都执行同一条 placement-new 表达式，malloc<int>() 的值初始化
     *       等既有语义不变。value_type 构造函数、以及自定义 Allocator::allocate 的
     *       其他异常原样传播。任意构造异常都会精确恢复 bump/free-list；若刚扩展
     *       新块，也会摘除并释放该块。
     */
    template<typename... Args>
    pointer malloc(Args&&... args)
    {
        static_assert(std::is_constructible<value_type, Args&&...>::value,
                      "传入参数无法构造 CMemoryObjectPool 的 value_type");

        // 第一标签取"平凡且 nothrow"而不是单独的平凡：CWG1778 允许
        // `S() noexcept(false) = default;` 这类平凡但非 nothrow 的构造，
        // 单独按平凡分派会落入不存在的 (真,假) 组合导致编译失败。
        // 合取后组合空间闭合为 (真,真)、(假,真)、(假,假)，恰好对应三个重载；
        // 平凡但显式声明可抛的边角类型安全落入完整事务路径。
        return mallocImpl(
            std::integral_constant<bool,
                std::is_trivially_constructible<value_type, Args&&...>::value
                && std::is_nothrow_constructible<value_type, Args&&...>::value>(),
            std::integral_constant<bool,
                std::is_nothrow_constructible<value_type, Args&&...>::value>(),
            std::forward<Args>(args)...);
    }

    /**
     * @brief 析构并归还一个当前活动对象。
     * @param[in] value 必须是本池 malloc() 返回且尚未释放的对象首地址；允许为 nullptr。
     * @retval true 对象已析构且槽位已归还。
     * @retval false value 为空、来自外部、不是槽首、已释放，或发生同池重入。
     * @note 实现不传播 C++ 异常，但保留旧版未声明 noexcept 的公开签名。未定义 NDEBUG
     *       时，高水位范围内可确认的未对齐地址或非活动槽会触发 assert。
     * @note 本入口通过静态直连 hook 析构对象：平凡析构类型不生成任何析构调用，
     *       非平凡析构直接调用 ~value_type()，都不经过每次释放一次的虚函数派发。
     *       经由 Base 指针/引用调用继承的 free(void*) 仍走虚 hook，行为等价。
     */
    bool free(pointer value)
    {
        return Base::freeWithHook(value, ObjectFreeHook());
    }

    /**
     * @brief 接管另一个完整空对象池的全部物理块。
     * @param[in,out] other 来源池；成功后不再拥有物理块。自合并无操作。
     * @throws std::logic_error 任一池处于受保护操作、元数据损坏或来源池非空。
     * @throws std::invalid_argument 两池对齐或 allocator owner 不兼容。
     * @note 有状态 allocator 比较的其他异常会在所有权转移前原样传播。
     */
    void merge(CMemoryObjectPool& other)
    {
        Base::mergePool(other);
    }

    /**
     * @brief 把当前池中的全部完整空物理块转移到目标池。
     * @param[in,out] target 接收物理块的目标池；可以已经拥有物理块。
     * @return 从来源链摘下的完整空物理块数量，单位：块；自分离或空来源返回 0。
     * @throws std::logic_error 任一池处于受保护操作或元数据损坏。
     * @throws std::invalid_argument 两池对齐或 allocator owner 不兼容。
     * @note 当前池可以仍有活动对象；有状态 allocator 比较异常会在修改块链前原样传播。
     */
    size_t split(CMemoryObjectPool& target)
    {
        return Base::splitPool(target);
    }

    /**
     * @brief 交换两个对象池的 allocator、块链、热点和布局状态。
     * @param[in,out] other 另一个对象池；允许自交换。
     * @throws std::logic_error 任一池处于受保护操作。
     * @throws std::invalid_argument allocator 不传播且两个实例不兼容。
     * @note 有状态 allocator 比较的其他异常会在交换前原样传播；传播型 swap 被约束为 noexcept。
     */
    void swap(CMemoryObjectPool& other)
    {
        Base::swapPool(other);
    }

protected:
    /**
     * @brief Base 已清活动位后析构单个 value_type 的释放 hook。
     * @param[in] value 待析构对象首地址。
     * @note 本函数为 noexcept。平凡析构类型通过编译期分派成为空操作。
     */
    void beforeFree(void* value) noexcept override
    {
        destroyOne(reinterpret_cast<pointer>(value),
                   std::integral_constant<bool,
                   std::is_trivially_destructible<value_type>::value>());
    }

    /**
     * @brief 析构一个物理块中全部仍有活动位的 value_type。
     * @param[in,out] head 待集中清理的物理块。
     * @note 本函数为 noexcept。Base 随后统一重置块元数据或释放物理块。
     */
    void destroyBlock(MemoryHead* head) noexcept override
    {
        destroyBlockImpl(head,
                         std::integral_constant<bool,
                         std::is_trivially_destructible<value_type>::value>());
    }

private:
    /**
     * @brief 本类 free() 静态直连使用的对象释放 hook。
     *
     * 非平凡析构直接调用 ~value_type() 并保留 releasing 重入保护；平凡析构的
     * operator() 为空操作，Base 的 freeWithHook() 会在编译期一并裁掉状态切换。
     */
    struct ObjectFreeHook
    {
        // 非平凡析构会执行用户代码, 需要 releasing 重入保护；平凡析构整段裁掉。
        using invokes_user_code = std::integral_constant<bool,
            !std::is_trivially_destructible<value_type>::value>;

        /** @brief 按编译期标签直接析构一个 value_type，不经过虚派发。 */
        void operator()(void* value) const noexcept
        {
            destroyOne(reinterpret_cast<pointer>(value),
                       std::integral_constant<bool,
                       std::is_trivially_destructible<value_type>::value>());
        }
    };

    /**
     * @brief 平凡且 nothrow 构造类型的 malloc 快路径分派。
     * @tparam Args 构造参数类型包。
     * @param[in] trivialTag std::true_type，表示构造平凡且 nothrow，不运行任何用户代码。
     * @param[in] nothrowTag std::true_type，构造不抛异常。
     * @param[in] args 完美转发给 value_type 的构造参数。
     * @return 已完成构造的活动对象指针。
     * @throws std::logic_error 同池重入或内部 free-list 计数损坏。
     * @throws std::length_error / std::runtime_error / std::bad_alloc 扩块失败。
     * @note 平凡构造既不可能抛异常也不可能重入本池，因此直接复用字节池的
     *       "预留即提交"路径（Base::alloc()），随后原地开始对象生命周期；提交与
     *       构造之间没有任何用户代码窗口，query/free 仍然只能看到完整对象。
     */
    template<typename... Args>
    pointer mallocImpl(std::true_type trivialTag, std::true_type nothrowTag, Args&&... args)
    {
        (void)trivialTag;
        (void)nothrowTag;
        pointer result = reinterpret_cast<pointer>(Base::alloc());
        // 与其余路径完全相同的 placement-new 表达式，保证参数转发和值初始化语义。
        ::new (static_cast<void*>(result)) value_type(std::forward<Args>(args)...);
        return result;
    }

    /**
     * @brief 不抛异常的非平凡构造的 malloc 分派。
     * @tparam Args 构造参数类型包。
     * @param[in] trivialTag std::false_type，构造会执行用户代码。
     * @param[in] nothrowTag std::true_type，构造声明不抛异常。
     * @param[in] args 完美转发给 value_type 的构造参数。
     * @return 已完成构造的活动对象指针。
     * @throws std::logic_error 同池重入或内部 free-list 计数损坏。
     * @throws std::length_error / std::runtime_error / std::bad_alloc 扩块失败。
     * @note 用户构造代码仍可能重入本池，因此保留 constructing 事务保护；但构造
     *       不会抛异常，省去 try/catch 与回滚路径，提交即完成。
     */
    template<typename... Args>
    pointer mallocImpl(std::false_type trivialTag, std::true_type nothrowTag, Args&&... args)
    {
        (void)trivialTag;
        (void)nothrowTag;
        // 预留阶段尚未写活动位，构造函数重入同一池会被 Base 的 constructing 状态拒绝。
        typename Base::RawReservation reservation = Base::beginObjectConstruction();
        pointer result = reinterpret_cast<pointer>(reservation.address);
        ::new (static_cast<void*>(result)) value_type(std::forward<Args>(args)...);
        // 构造声明 noexcept，控制流到达此处即构造成功，可直接提交活动位。
        Base::commitObjectConstruction(reservation);
        return result;
    }

    /**
     * @brief 可能抛异常的构造的 malloc 完整事务分派。
     * @tparam Args 构造参数类型包。
     * @param[in] trivialTag std::false_type，构造会执行用户代码。
     * @param[in] nothrowTag std::false_type，构造可能抛异常。
     * @param[in] args 完美转发给 value_type 的构造参数。
     * @return 已完成构造的活动对象指针。
     * @throws std::logic_error 同池重入或内部 free-list 计数损坏。
     * @throws std::length_error / std::runtime_error / std::bad_alloc 扩块失败。
     * @note value_type 构造异常触发按槽位来源精确回滚后原样传播，行为与旧版一致。
     */
    template<typename... Args>
    pointer mallocImpl(std::false_type trivialTag, std::false_type nothrowTag, Args&&... args)
    {
        (void)trivialTag;
        (void)nothrowTag;
        // 预留阶段尚未写活动位，构造函数重入同一池会被 Base 的 constructing 状态拒绝。
        typename Base::RawReservation reservation = Base::beginObjectConstruction();
        pointer result = reinterpret_cast<pointer>(reservation.address);
        try
        {
            ::new (static_cast<void*>(result)) value_type(std::forward<Args>(args)...);
        }
        catch (...)
        {
            // 对象生命周期未成功开始，只撤销 raw slot 预留，不调用 value_type 析构函数。
            Base::rollbackObjectConstruction(reservation);
            throw;
        }

        // 只有完整构造成功后才置活动位，使 query/free 永远不会看到半构造对象。
        Base::commitObjectConstruction(reservation);
        return result;
    }

    /**
     * @brief 平凡析构类型的单对象空分派。
     * @param[in] value 对象地址；平凡析构无需访问。
     * @param[in] tag std::true_type 标签。
     * @note 本函数为 noexcept，不执行任何工作。
     */
    static void destroyOne(pointer value, std::true_type tag) noexcept
    {
        (void)value;
        (void)tag;
    }

    /**
     * @brief 非平凡析构类型的单对象分派。
     * @param[in] value 指向一个活动 value_type。
     * @param[in] tag std::false_type 标签。
     * @note 本函数为 noexcept；类级 static_assert 已要求 value_type 析构函数 noexcept。
     */
    static void destroyOne(pointer value, std::false_type tag) noexcept
    {
        (void)tag;
        value->~value_type();
    }

    /**
     * @brief 平凡析构类型的整块空分派。
     * @param[in] head 物理块头；无需逐槽访问。
     * @param[in] tag std::true_type 标签。
     * @note 本函数为 noexcept；Base 随后的 resetBlock() 会一次性清空整份活动位图。
     */
    void destroyBlockImpl(MemoryHead* head, std::true_type tag) noexcept
    {
        (void)head;
        (void)tag;
    }

    /**
     * @brief 遍历高水位并析构整块中每个仍活动的非平凡对象。
     * @param[in,out] head 待清理物理块。
     * @param[in] tag std::false_type 标签。
     * @note 本函数为 noexcept。只遍历 [dataBegin, item) 的完整槽位，跳过活动位为 0 的洞。
     */
    void destroyBlockImpl(MemoryHead* head, std::false_type tag) noexcept
    {
        (void)tag;
        const size_t slotSize = Base::slotSizeValue();
        const size_t highWater = static_cast<size_t>(head->item - head->dataBegin) / slotSize;

        // 高水位之后从未开始过对象生命周期，因此只需检查已经覆盖的槽位。
        for (size_t index = 0; index < highWater; ++index)
        {
            if (!Base::slotIndexInUse(head, index))
                continue;

            // 位图本来就按槽索引编号，循环直接使用 index，避免每个对象重复执行除法。
            Base::setSlotIndexActive(head, index, false);
            const size_t offset = index * slotSize;
            destroyOne(reinterpret_cast<pointer>(head->dataBegin + offset), std::false_type());
        }
    }
};

/**
 * @brief 运行时固定槽位尺寸的原始字节池。
 *
 * malloc() 返回地址至少满足 alignof(std::max_align_t)，每个池实例可以在完整空状态下
 * 通过 resize_slot() 改变槽位尺寸。池只管理 raw storage 和活动位，不构造、不清零，
 * 也不替调用者析构放置在槽位中的对象。
 *
 * @tparam Allocator 分配页对齐物理块的内部单字节 allocator 类型。
 */
template<class Allocator = CMemoryPoolAllocator<uint8_t>>
class CMemoryBytePool : public CMemoryPoolBase<Allocator>
{
private:
    using Base = CMemoryPoolBase<Allocator>; // 公共块管理实现。

public:
    using allocator_type = Allocator; // 页对齐物理块 allocator 类型。

    /**
     * @brief 构造延迟初始化的默认字节池。
     * @post 不建立物理块；原始请求槽尺寸为 sizeof(void*)，实际 get_slot() 会再按
     *       内部最小节点尺寸和 alignof(std::max_align_t) 向上取整。
     * @note 自定义 Allocator 默认构造若抛异常，该异常原样传播。
     */
    explicit CMemoryBytePool()
        : Base(sizeof(void*), alignof(std::max_align_t), 0)
    {
    }

    /**
     * @brief 按旧版签名构造字节池。
     * @param[in] slotSize 调用方请求的单槽尺寸，单位：字节。
     * @param[in] count 首块至少容纳的槽位数量，单位：个；默认 4096，显式 0 表示延迟初始化。
     * @throws std::invalid_argument slotSize 为 0。
     * @throws std::length_error 尺寸运算溢出或超过 allocator 上限。
     * @throws std::runtime_error 无法取得有效系统页大小。
     * @throws std::bad_alloc 物理块分配失败或页对齐契约不成立。
     * @note 保留 `explicit CMemoryBytePool(size_t, size_t = 0x1000)` 旧公开签名；自定义
     *       Allocator 默认构造或 allocate 的其他异常原样传播。
     */
    explicit CMemoryBytePool(size_t slotSize, size_t count = 0x1000)
        : Base(slotSize, alignof(std::max_align_t), count)
    {
    }

    /**
     * @brief 复制指定 allocator，按指定槽尺寸构造并立即建立默认首块。
     * @param[in] slotSize 调用方请求的单槽尺寸，单位：字节。
     * @param[in] allocator 用于复制构造内部 allocator 的实例。
     * @throws std::invalid_argument slotSize 为 0。
     * @throws std::length_error 尺寸运算溢出或超过 allocator 上限。
     * @throws std::runtime_error 无法取得有效系统页大小。
     * @throws std::bad_alloc 物理块分配失败或页对齐契约不成立。
     * @note 保留旧版 allocator 单参数容量构造的立即初始化语义；自定义异常原样传播。
     */
    CMemoryBytePool(size_t slotSize, const allocator_type& allocator)
        : Base(slotSize, alignof(std::max_align_t), 0, allocator)
    {
        // Base 的 count=0 先完成布局和 allocator 构造，再显式按默认预算建立首块。
        Base::init();
    }

    /**
     * @brief 使用指定 allocator、槽尺寸和首块槽数量构造字节池。
     * @param[in] slotSize 调用方请求的单槽尺寸，单位：字节。
     * @param[in] count 首块至少容纳的槽位数量，单位：个；0 表示延迟初始化。
     * @param[in] allocator 用于复制构造内部 allocator 的实例。
     * @throws std::invalid_argument slotSize 为 0。
     * @throws std::length_error 尺寸运算溢出或超过 allocator 上限。
     * @throws std::runtime_error 无法取得有效系统页大小。
     * @throws std::bad_alloc 物理块分配失败或页对齐契约不成立。
     * @note 自定义 Allocator 复制构造或 allocate 的其他异常原样传播。
     */
    CMemoryBytePool(size_t slotSize, size_t count, const allocator_type& allocator)
        : Base(slotSize, alignof(std::max_align_t), count, allocator)
    {
    }

    /** @brief 禁止复制构造，确保物理块链保持唯一所有权。 */
    CMemoryBytePool(const CMemoryBytePool&) = delete;

    /**
     * @brief 移动接管来源字节池的 allocator、槽尺寸、块链和热点状态。
     * @param[in,out] other 来源池；成功后为空且可析构或重新赋值。默认 allocator 下可
     *                      直接复用；自定义 allocator 取决于其移动后状态契约。
     * @pre other 必须处于 idle；违反前置条件会按 Base 的 public noexcept 兼容移动规则
     *      触发断言或终止。
     * @note 保留旧版字节池隐式移动构造的 noexcept 属性。Allocator 移动构造由 Base
     *       编译期约束为 noexcept；本操作为 O(1)。
     */
    CMemoryBytePool(CMemoryBytePool&& other) noexcept
        : Base(std::move(other))
    {
    }

    /**
     * @brief 归还全部物理块并结束字节池生命周期。
     * @note 本函数为 noexcept。不会调用槽位中由用户 placement-new 的对象析构函数；
     *       用户必须在释放槽位或销毁池之前自行结束这些对象的生命周期。
     */
    ~CMemoryBytePool() noexcept override
    {
        Base::destroy();
    }

    /** @brief 禁止复制赋值，避免多个池共享同一物理块链。 */
    CMemoryBytePool& operator=(const CMemoryBytePool&) = delete;

    /**
     * @brief 销毁当前池物理块后移动接管来源池。
     * @param[in,out] other 来源池；成功后为空且可析构或重新赋值。默认 allocator 下可
     *                      直接复用；自定义 allocator 取决于其移动后状态。自移动无操作。
     * @return 当前对象 *this。
     * @throws std::logic_error 任一池处于受保护操作。
     * @throws std::invalid_argument allocator 不传播且两个实例不兼容。
     * @note 有状态 allocator 比较的其他异常会在目标旧块销毁前原样传播；提交阶段不抛。
     */
    CMemoryBytePool& operator=(CMemoryBytePool&& other)
    {
        Base::moveAssignPool(std::move(other));
        return *this;
    }

    /**
     * @brief 分配一个已提交活动位的原始字节槽位。
     * @return 原始槽位首地址；可用空间为 get_slot() 字节，地址至少满足 max_align_t。
     * @throws std::logic_error 同池重入或内部 free-list 计数损坏。
     * @throws std::length_error 扩块尺寸运算溢出或超过 allocator 上限。
     * @throws std::runtime_error 无法取得有效系统页大小。
     * @throws std::bad_alloc 新物理块分配失败或页对齐契约不成立。
     * @note 本函数不构造、不清零槽内容；自定义 Allocator::allocate 的其他异常原样传播。
     */
    void* malloc()
    {
        return Base::alloc();
    }

    /**
     * @brief 归还一个当前活动的原始字节槽位。
     * @param[in] value 本池 malloc() 返回且尚未释放的槽位首地址；允许为 nullptr。
     * @retval true 槽位活动位已清除并成功归还。
     * @retval false 地址为空、来自外部、不是槽首、已释放，或发生同池重入。
     * @note 实现不传播 C++ 异常，但保留旧版未声明 noexcept 的公开签名，也不析构用户
     *       放在槽中的对象。未定义 NDEBUG 时可确认的非法槽会触发 assert。
     * @note 字节池释放不运行任何用户代码，因此走空 hook 的静态路径：免去每次释放
     *       一次的虚函数派发，也免去 releasing/idle 状态切换。
     */
    bool free(void* value)
    {
        return Base::freeWithHook(value, typename Base::TrivialFreeHook());
    }

    /**
     * @brief 在完整空池上修改最终槽位尺寸。
     * @param[in] slotSize 新的原始槽尺寸，单位：字节；最终值按 max_align_t 向上取整。
     * @throws std::invalid_argument slotSize 为 0。
     * @throws std::length_error 槽尺寸向上对齐溢出或最终尺寸超过 UINT32_MAX。
     * @throws std::logic_error 池处于受保护操作、元数据损坏或仍存在活动槽位。
     * @post 旧空块按新尺寸重新解释；连一个新槽都容纳不下的块被释放，不主动建立新块。
     */
    void resize_slot(size_t slotSize)
    {
        Base::resizeSlot(slotSize);
    }

    /**
     * @brief 接管另一个完整空字节池的全部物理块。
     * @param[in,out] other 来源池；成功后不再拥有物理块。自合并无操作。
     * @throws std::logic_error 任一池处于受保护操作、元数据损坏或来源池非空。
     * @throws std::invalid_argument 两池槽对齐或 allocator owner 不兼容。
     * @note 两池槽尺寸可以不同，转移后按当前池尺寸重新解释完整空块；allocator 比较异常
     *       会在所有权转移前原样传播。
     */
    void merge(CMemoryBytePool& other)
    {
        Base::mergePool(other);
    }

    /**
     * @brief 把当前池中的全部完整空物理块转移到目标字节池。
     * @param[in,out] target 接收块的目标池；可使用不同槽尺寸，但槽地址对齐必须相同。
     * @return 从来源链摘下的完整空物理块数量，单位：块；自分离或空来源返回 0。
     * @throws std::logic_error 任一池处于受保护操作或元数据损坏。
     * @throws std::invalid_argument 两池对齐或 allocator owner 不兼容。
     * @note 当前池可以仍有活动槽；只摘取完整空块。目标尺寸下容量为 0 的摘取块会被
     *       立即归还，因此返回值可能大于 target 最终增加的块数。allocator 比较异常
     *       在修改块链前传播。
     */
    size_t split(CMemoryBytePool& target)
    {
        return Base::splitPool(target);
    }

    /**
     * @brief 交换两个字节池的 allocator、运行时槽尺寸、块链和热点状态。
     * @param[in,out] other 另一个字节池；允许自交换。
     * @throws std::logic_error 任一池处于受保护操作。
     * @throws std::invalid_argument allocator 不传播且两个实例不兼容。
     * @note 有状态 allocator 比较异常在交换前原样传播；传播型 swap 被约束为 noexcept。
     */
    void swap(CMemoryBytePool& other)
    {
        Base::swapPool(other);
    }
};

NAMESPACE_MEMORYPOOL_END
