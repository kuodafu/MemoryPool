#pragma once

#include "CMemoryAllocator.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

// 合并或分离块链以后是否按物理块尺寸排序: 0 = 保持原顺序, 1 = 从小到大排序。
#ifndef MEMORYPOOL_SORT_AFTER_MERGE_SPLIT
#define MEMORYPOOL_SORT_AFTER_MERGE_SPLIT 0
#endif

#if defined(_MSC_VER)
    #if defined(MEMORYPOOL_TEST_DIAGNOSTICS)
        #pragma detect_mismatch("kuodafu.memorypool.diagnostics", "1")
    #else
        #pragma detect_mismatch("kuodafu.memorypool.diagnostics", "0")
    #endif
    #if MEMORYPOOL_SORT_AFTER_MERGE_SPLIT
        #pragma detect_mismatch("kuodafu.memorypool.sort_after_merge_split", "1")
    #else
        #pragma detect_mismatch("kuodafu.memorypool.sort_after_merge_split", "0")
    #endif
#endif

NAMESPACE_MEMORYPOOL_BEGIN

/**
 * @brief 固定槽位内存池的公共实现。
 *
 * 本类负责页对齐物理块、块链、槽位活动位图、空闲链表和分配热点, 具体槽位中
 * 对象的构造与析构规则由派生类实现。每个物理块在 MemoryHead 后紧接一份持久
 * 活动位图, 位图占位结束后才是槽位数据区, 因而能够可靠识别重复释放、未对齐
 * 地址以及尚未提交构造的槽位。
 *
 * 活动位图与 free-list 并不重复: 位图是槽位生命周期的真实性记录, free-list 是
 * 已释放非尾空洞的 O(1) 地址索引。删除 free-list 会迫使分配扫描“一槽一 bit”的
 * 位图寻找空洞, 在碎片场景退化为 O(槽位数), 因此吞吐优先的实现同时保留两者。
 *
 * @tparam Allocator 内部字节分配器类型。其 value_type 必须恰好为 1 字节, pointer
 *                   必须是原始指针, deallocate 在运行时不得抛出异常, 并且 allocate
 *                   返回的物理块首地址必须按真实系统页大小对齐。
 *
 * @note 本内存池是可供不同上层复用的通用固定槽分配器, 不绑定 ListView。单个池
 *       实例不是线程安全的, 任意调用方都必须在池外完成串行化；alloc/free 热路径
 *       不包含锁。
 */
template<class Allocator = CMemoryPoolAllocator<uint8_t>>
class CMemoryPoolBase
{
public:
    using allocator_type = Allocator; // 对外公开的底层字节分配器类型。

    /** @brief 单个物理块的只读布局快照, 供诊断和测试读取。 */
    struct BlockInfo
    {
        const void* blockBase;      // 物理块首地址, 按系统页对齐。
        size_t      blockSize;      // 物理块总尺寸, 单位: 字节；是系统页大小的整数倍。
        const void* dataBegin;      // 槽位区首地址, 满足池的槽位对齐要求。
        const void* dataEnd;        // 槽位区尾后地址, 当前等于物理块尾后地址。
        size_t      bitmapBytes;    // 位图对齐占位跨度, 单位: 字节；包含位图后的对齐补齐。
        size_t      slotCapacity;   // 当前槽位尺寸下可容纳的槽位数量, 单位: 个。
        size_t      highWaterSlots; // bump 高水位已经覆盖的槽位数量, 单位: 个。
        size_t      freeSlots;      // free-list 中的空闲槽位数量, 单位: 个。
    };

protected:
    using AllocatorTraits = std::allocator_traits<Allocator>; // allocator_traits 适配类型。
    using AllocatorValue = typename AllocatorTraits::value_type; // allocator 元素类型。
    using AllocatorPointer = typename AllocatorTraits::pointer; // allocator 返回的指针类型。
    using AllocatorSize = typename AllocatorTraits::size_type; // allocator 数量类型。
    using PropagateOnMoveAssignment =
        typename AllocatorTraits::propagate_on_container_move_assignment; // 移动赋值传播策略。
    using PropagateOnSwap =
        typename AllocatorTraits::propagate_on_container_swap; // swap 传播策略。
    using BytePointer = uint8_t*; // 块内按字节计算地址时使用的原始指针类型。

    static_assert(sizeof(AllocatorValue) == 1,
                  "CMemoryPoolBase 的 allocator::value_type 必须恰好占 1 字节");
    static_assert(std::is_same<AllocatorPointer, AllocatorValue*>::value,
                  "CMemoryPoolBase 只支持返回原始指针的内部字节 allocator");
    static_assert(std::is_nothrow_move_constructible<Allocator>::value,
                  "内存池 allocator 的移动构造必须 noexcept, 避免块链与 owner 失配");
    static_assert(!PropagateOnMoveAssignment::value
                  || std::is_nothrow_move_assignable<Allocator>::value,
                  "传播型内存池 allocator 的移动赋值必须 noexcept");
    static_assert(!PropagateOnSwap::value
                  || noexcept(std::swap(std::declval<Allocator&>(),
                                        std::declval<Allocator&>())),
                  "传播型内存池 allocator 的 swap 必须 noexcept");

    /** @brief 已释放槽位复用其自身存储形成的单向链表节点。 */
    struct ListNode
    {
        ListNode* next; // 同一物理块中下一个已释放槽位；nullptr 表示链尾。
    };

    /** @brief 位于每个页对齐物理块最前端的块级元数据。 */
    struct MemoryHead
    {
        MemoryHead* next;       // 块链中的下一块；nullptr 表示真实链尾。
        size_t      size;       // 整个物理块尺寸, 单位: 字节；始终按系统页取整。
        BytePointer dataBegin;  // 槽位区首地址, 满足 get_alignment() 返回的对齐值。
        BytePointer item;       // bump 高水位, 即下一个从未使用槽位的首地址。
        BytePointer dataEnd;    // 槽位区尾后地址, 当前等于物理块尾后地址。
        size_t      stateBytes; // MemoryHead 后的位图对齐占位跨度, 单位: 字节。
        ListNode*   freeList;   // 非尾部释放槽组成的单向链表表头。
        size_t      freeCount;  // freeList 节点数量, 单位: 槽位个数。
    };

    using PMEMORY_HEAD = MemoryHead*; // 兼容现有派生代码的块头指针别名。
    using PLIST_NODE = ListNode*; // 兼容现有派生代码的空闲节点指针别名。

    /** @brief 防止对象构造、对象释放和批量清理发生同池重入的操作状态。 */
    enum class Operation : uint8_t
    {
        idle,         // 当前没有受保护操作。
        constructing, // 已预留槽位, 正在调用用户对象构造函数。
        releasing,    // 已清活动位, 正在调用单个用户对象析构函数。
        clearing,     // 正在析构所有活动对象并重置全部块。
        destroying,   // 正在析构活动对象并归还全部物理块。
        managing      // 正在调用 allocator 或重组块链，拒绝同池修改重入。
    };

    /** @brief 构造事务所使用槽位的来源, 用于异常时精确回滚。 */
    enum class ReservationOrigin : uint8_t
    {
        bump,    // 槽位来自当前块尚未使用的连续区。
        freeList,// 槽位来自当前块或旧块的空闲链表。
        newBlock // 槽位来自本次刚追加的新物理块。
    };

    using PoolStateFlags = uint8_t; // 保存扫描提示位的无符号类型。

    /** @brief stateFlags_ 中可独立开关的布尔状态位。 */
    enum class PoolStateFlag : PoolStateFlags
    {
        scanOtherBlocks = PoolStateFlags(1) << 0 // 当前块耗尽时是否还需要扫描其他旧块。
    };

    /** @brief 尚未提交活动位的原始槽位预留记录。 */
    struct RawReservation
    {
        MemoryHead*       head;              // 本次预留槽位所在的物理块。
        BytePointer       address;           // 预留槽位首地址。
        MemoryHead*       previousCurrent;   // 事务开始前的分配热点块。
        MemoryHead*       previousTail;      // 事务开始前的真实链尾。
        ReservationOrigin origin;            // 预留槽位的来源类别。
        PoolStateFlags    previousScanFlags; // 事务开始前保存的旧块扫描位, 仅使用 bit 0。
    };

    /** @brief 根据一个物理块总尺寸和当前槽尺寸计算出的块内布局。 */
    struct BlockLayout
    {
        std::uintptr_t bitmapBegin;  // MemoryHead 尾后、有效位图开始的整数地址。
        std::uintptr_t dataBegin;    // 位图和地址补齐之后的首槽地址。
        std::uintptr_t blockEnd;     // 整个页对齐物理块的尾后地址。
        size_t         rawStateBytes;// 按候选槽数量计算的有效位图字节数。
        size_t         stateBytes;   // bitmapBegin 到 dataBegin 的总占位跨度。
        size_t         slotCapacity; // [dataBegin, blockEnd) 可容纳的完整槽位数量。
    };

    static constexpr size_t initialPayloadBudget_ = size_t(1) << 20;  // 初始块 payload 预算, 单位: 字节（1 MiB）。
    static constexpr size_t growthPayloadBudget_  = size_t(16) << 20; // 扩展块 payload 上限, 单位: 字节（16 MiB）。
    static constexpr size_t maximumInitialSlots_  = 4096; // 自动初始化时的最大槽位数量, 单位: 个。

    // 紧凑标量放在一起；默认空 allocator_ 会占用快速除法常量后的自然补齐空间。
    // 不显式声明 reserve：编译器会自动满足后续指针对齐，显式保留反而会放大对象。
    uint32_t       slotSize_;           // 最终对齐后的单槽尺寸, 单位: 字节；最大 UINT32_MAX。
    uint8_t        alignmentShift_;     // 槽位对齐值的 log2；完整值按 1 << alignmentShift_ 推导。
    PoolStateFlags stateFlags_;         // bit 0 为扫描提示, 其余位保留。
    uint8_t        operation_;          // Operation 的直接字节编码, 热路径单字节读写。
    uint8_t        slotDivisorShift_;   // slotSize_ 中因子 2 的个数, 即二进制尾零位数。
    size_t         slotDivisorInverse_; // slotSize_ 奇数部分在模 2^N 下的乘法逆, N 为 size_t 位宽。
    size_t         slotDivisorLimit_;   // SIZE_MAX / slotSize_, 快速除法整除判定的商上界。
    Allocator      allocator_;          // 分配和归还页对齐物理块的 allocator 实例。
    MemoryHead*    memory_;             // 物理块链首；nullptr 表示尚未建立物理块。
    MemoryHead*    tail_;               // 真实块链尾, 用于 O(1) 追加新块。
    MemoryHead*    current_;            // alloc/free 首先访问的热点物理块。
#if defined(MEMORYPOOL_TEST_DIAGNOSTICS)
    size_t diagnosticLastScanCount_; // 最近一次分配扫描的旧块数量, 单位: 块；仅测试构建存在。
#endif

    /**
     * @brief 构造使用默认 allocator 的延迟或立即初始化内存池。
     *
     * @param[in] slotSize 调用方请求的单槽尺寸, 单位: 字节；保存前至少扩到
     *                     sizeof(ListNode), 再按规范化后的槽位对齐值向上取整。
     * @param[in] slotAlignment 请求的槽位地址对齐值, 单位: 字节；必须是非零的 2 次幂,
     *                          小于 alignof(ListNode) 时自动提升。
     * @param[in] count 初始槽位数量, 单位: 个；0 表示延迟建立首个物理块。
     *
     * @throws std::invalid_argument slotSize 为 0, 或 slotAlignment 不是非零的 2 次幂。
     * @throws std::length_error 尺寸乘法、加法、对齐计算溢出, 或超过 allocator 上限。
     * @throws std::runtime_error 无法取得有效的系统页大小。
     * @throws std::bad_alloc 底层物理块分配失败或返回地址不满足系统页对齐。
     * @note 自定义 Allocator 的默认构造或 allocate 若抛出其他异常, 该异常原样传播。
     */
    explicit CMemoryPoolBase(size_t slotSize, size_t slotAlignment, size_t count)
        : slotSize_(0)
        , alignmentShift_(0)
        , stateFlags_(0)
        , operation_(static_cast<uint8_t>(Operation::idle))
        , slotDivisorShift_(0)
        , slotDivisorInverse_(0)
        , slotDivisorLimit_(0)
        , allocator_()
        , memory_(nullptr)
        , tail_(nullptr)
        , current_(nullptr)
#if defined(MEMORYPOOL_TEST_DIAGNOSTICS)
        , diagnosticLastScanCount_(0)
#endif
    {
        // allocator 构造完成后再验证布局参数，保持默认/自定义 allocator 构造的异常顺序一致。
        const size_t normalisedAlignment = normaliseAlignment(slotAlignment);
        alignmentShift_ = alignmentShift(normalisedAlignment);
        slotSize_ = alignSlot(slotSize, normalisedAlignment);
        // 槽尺寸确定后立即生成快速除法常量, 热路径的槽索引换算不再执行硬件除法。
        updateSlotDivisionConstants();
        if (count != 0)
            init(count);
    }

    /**
     * @brief 构造使用指定 allocator 的延迟或立即初始化内存池。
     *
     * @param[in] slotSize 调用方请求的单槽尺寸, 单位: 字节。
     * @param[in] slotAlignment 请求的槽位地址对齐值, 单位: 字节；必须是非零的 2 次幂。
     * @param[in] count 初始槽位数量, 单位: 个；0 表示延迟建立首个物理块。
     * @param[in] allocator 用于复制构造内部 allocator_ 的分配器实例。
     *
     * @throws std::invalid_argument slotSize 为 0, 或 slotAlignment 不是非零的 2 次幂。
     * @throws std::length_error 尺寸运算溢出或物理块超过 allocator 上限。
     * @throws std::runtime_error 无法取得有效的系统页大小。
     * @throws std::bad_alloc 底层物理块分配失败或页对齐契约不成立。
     * @note 自定义 Allocator 的复制构造或 allocate 若抛出其他异常, 该异常原样传播。
     */
    CMemoryPoolBase(size_t slotSize, size_t slotAlignment, size_t count,
                    const Allocator& allocator)
        : slotSize_(0)
        , alignmentShift_(0)
        , stateFlags_(0)
        , operation_(static_cast<uint8_t>(Operation::idle))
        , slotDivisorShift_(0)
        , slotDivisorInverse_(0)
        , slotDivisorLimit_(0)
        , allocator_(allocator)
        , memory_(nullptr)
        , tail_(nullptr)
        , current_(nullptr)
#if defined(MEMORYPOOL_TEST_DIAGNOSTICS)
        , diagnosticLastScanCount_(0)
#endif
    {
        // allocator 构造完成后再计算槽尺寸；若后续建块失败, 成员会按正常栈展开销毁。
        const size_t normalisedAlignment = normaliseAlignment(slotAlignment);
        alignmentShift_ = alignmentShift(normalisedAlignment);
        slotSize_ = alignSlot(slotSize, normalisedAlignment);
        // 槽尺寸确定后立即生成快速除法常量, 热路径的槽索引换算不再执行硬件除法。
        updateSlotDivisionConstants();
        if (count != 0)
            init(count);
    }

public:
    /**
     * @brief 接管另一个池的 allocator、块链、布局和热点状态。
     *
     * @param[in,out] other 来源池。成功后其块链为空、状态恢复 idle；始终可析构或重新赋值。
     *                      默认无状态 allocator 下可直接重新使用；自定义 allocator 能否
     *                      直接再次分配取决于其移动后状态契约。
     *
     * @pre other 必须处于 idle。未定义 NDEBUG 时违反前置条件会触发内部断言；当前兼容
     *      签名为 noexcept，若状态检查产生异常，程序会按 noexcept 规则终止。
     * @note 保留旧版 public noexcept 移动构造签名。本模板在编译期要求 Allocator 移动
     *       构造 noexcept；正常 idle 路径不逐块遍历、不分配物理内存，复杂度 O(1)。
     */
    CMemoryPoolBase(CMemoryPoolBase&& other) noexcept
        : slotSize_(other.slotSize_)
        , alignmentShift_(other.alignmentShift_)
        , stateFlags_(static_cast<PoolStateFlags>(
              other.stateFlags_ & static_cast<PoolStateFlags>(PoolStateFlag::scanOtherBlocks)))
        , operation_(static_cast<uint8_t>(Operation::idle))
        , slotDivisorShift_(other.slotDivisorShift_)
        , slotDivisorInverse_(other.slotDivisorInverse_)
        , slotDivisorLimit_(other.slotDivisorLimit_)
        , allocator_(moveAllocatorChecked(other))
        , memory_(other.memory_)
        , tail_(other.tail_)
        , current_(other.current_)
#if defined(MEMORYPOOL_TEST_DIAGNOSTICS)
        , diagnosticLastScanCount_(0)
#endif
    {
        // 所有权转移完成后一次性清空来源池的链和组合状态字节。
        other.memory_ = nullptr;
        other.tail_ = nullptr;
        other.current_ = nullptr;
        other.setStateFlag(PoolStateFlag::scanOtherBlocks, false);
        other.setOperation(Operation::idle);
#if defined(MEMORYPOOL_TEST_DIAGNOSTICS)
        other.diagnosticLastScanCount_ = 0;
#endif
    }

public:
    /**
     * @brief 使用旧版 public 二参数签名构造固定槽池。
     * @param[in] slotSize 调用方请求的单槽尺寸，单位：字节；按指针对齐规范化。
     * @param[in] count 首块至少容纳的槽位数量，单位：个；0 表示延迟初始化。
     * @throws std::invalid_argument slotSize 为 0。
     * @throws std::length_error 最终槽尺寸超过 UINT32_MAX、建块尺寸运算溢出，或超过
     *                           allocator::max_size()。
     * @throws std::runtime_error count 非 0 且无法取得有效系统页大小。
     * @throws std::bad_alloc count 非 0 且底层分配失败或块首不满足页对齐契约。
     * @note 该兼容入口供既有外部派生类和直接调用代码继续编译；新派生实现可使用
     *       protected 三/四参数构造函数显式传递槽首对齐要求。count 为 0 时不建块，
     *       除 allocator 构造和参数规范化外为 O(1)；立即建块还包含系统分配和位图清零。
     */
    explicit CMemoryPoolBase(size_t slotSize, size_t count)
        : CMemoryPoolBase(slotSize, alignof(void*), count)
    {
    }

    /** @brief 禁止复制池对象, 避免同一物理块链产生多个所有者。 */
    CMemoryPoolBase(const CMemoryPoolBase&) = delete;

    /**
     * @brief 基类虚析构函数。
     *
     * 具体派生池必须在自身析构阶段先调用 destroy(), 因为进入基类析构阶段后虚派发
     * 已不能再调用派生类的对象析构 hook。虚析构仅保证通过基类指针删除时先进入派生析构。
     *
     * @note 本函数为 noexcept, 不传播 C++ 异常。
     */
    virtual ~CMemoryPoolBase() noexcept
    {
        destroy();
    }

    /** @brief 禁止复制赋值, 避免物理块所有权和 allocator owner 失配。 */
    CMemoryPoolBase& operator=(const CMemoryPoolBase&) = delete;

    /**
     * @brief 初始化第一个内存块。
     *
     * @param[in] count 首块至少需要容纳的槽位数量, 单位: 个；0 表示根据约 1 MiB
     *                  payload 预算自动选择, 且自动值最多为 4096 个槽位。
     * @retval true 池此前已经初始化, 或本次成功建立首个物理块。
     * @retval false 当前正处于对象构造、单槽释放、clear 或 destroy 的重入保护期。
     *
     * @throws std::length_error 尺寸运算溢出或物理块超过 allocator::max_size()。
     * @throws std::runtime_error 无法取得有效的系统页大小。
     * @throws std::bad_alloc 底层分配失败, 或 allocator 返回的地址没有按系统页对齐。
     *
     * @note 自定义 Allocator::allocate 若抛出其他异常, 该异常原样传播。首块建立后再次
     *       调用不会扩大容量, count 也不会再次生效。
     */
    bool init(size_t count = 0x1000)
    {
        if (operationValue() != Operation::idle)
            return false;
        if (memory_)
            return true;

        const size_t actualCount = count == 0 ? defaultInitialCount() : count;
        ManagingOperationScope operation(*this);
        MemoryHead* head = mallocHead(actualCount);
        memory_ = head;
        tail_ = head;
        current_ = head;
        setStateFlag(PoolStateFlag::scanOtherBlocks, false);
        return true;
    }

    /**
     * @brief 判断地址是否是本池当前仍在使用的槽位首地址。
     *
     * @param[in] pointer 待查询地址；允许为 nullptr。非空地址必须恰好等于槽位首地址,
     *                    仅落入物理块、对齐填充、未使用区或位图区均不算活动槽位。
     * @retval true pointer 对应本池活动位图中仍为 1 的已提交槽位。
     * @retval false pointer 为空、来自其他池、不是槽首、已经释放, 或池处于重入保护期。
     *
     * @note 实现不传播 C++ 异常，但保留旧版未声明 noexcept 的公开签名。先检查
     *       current_ 热点块，最坏遍历全部物理块，复杂度 O(物理块数量)。
     */
    bool query(void* pointer) const
    {
        if (!pointer || !current_ || operationValue() != Operation::idle)
            return false;

        const BytePointer value = reinterpret_cast<BytePointer>(pointer);
        if (queryBlock(current_, value))
            return true;

        for (MemoryHead* head = memory_; head; head = head->next)
        {
            if (head != current_ && queryBlock(head, value))
                return true;
        }
        return false;
    }

    /**
     * @brief 归还一个当前仍在使用的槽位。
     *
     * Base 会先验证地址范围、槽边界和活动位, 再清除活动位并调用派生类 beforeFree()
     * 完成对象析构。尾部槽直接回退 bump 高水位, 其他槽复用其自身存储加入 free-list。
     *
     * @param[in] pointer 待归还的活动槽位首地址；允许传入 nullptr。
     * @retval true 活动位已经清除, 派生对象已经析构, 槽位已经成功归还。
     * @retval false pointer 为空、来自外部、不是槽首、已释放, 或发生同池重入。
     *
     * @note 实现不传播 C++ 异常，但保留旧版未声明 noexcept 的公开签名。未定义
     *       NDEBUG 时, 落入
     *       [dataBegin,item) 但未对齐的地址或活动位为 0 的槽首会触发 assert；assert
     *       不是 C++ 异常。尾槽回退、整块重置或地址复用后的历史错误无法由裸指针识别。
     * @note 先检查 current_。若失败地址仍落在当前物理块内, 可证明它不属于其他独立块,
     *       因而立即返回；否则最坏遍历 O(物理块数量)。成功释放为 O(1)：整块折叠只
     *       重置三个块头字段, 位图早已被逐次释放清零, 不再执行整块 memset。
     * @note 本入口通过虚 hook 调用派生类 beforeFree(), 保持直接以 Base 类型释放时的
     *       旧行为。具体对象池/字节池自身的 free() 走静态直连 hook, 不经过虚派发。
     */
    bool free(void* pointer)
    {
        return freeWithHook(pointer, VirtualFreeHook{this});
    }

    /**
     * @brief 析构所有活动对象并把所有物理块重置为可复用状态。
     *
     * @post 保留全部物理块；每块 bump 高水位回到 dataBegin, free-list 清空,
     *       freeCount 归零, 前置活动位图的有效字节全部清零, 地址补齐区不承载状态,
     *       current_ 回到链首。
     *
     * @note 实现不传播 C++ 异常，也不分配或归还物理块；为兼容旧接口，公开签名未声明
     *       noexcept。空池或清理期间的同池重入直接返回。字节池和平凡析构对象池为
     *       O(块数 + 位图字节数)，非平凡对象池还需 O(高水位槽数)。
     */
    void clear()
    {
        if (!memory_ || operationValue() != Operation::idle)
            return;

        setOperation(Operation::clearing);
        for (MemoryHead* head = memory_; head; head = head->next)
        {
            destroyBlock(head);
            resetBlock(head);
        }
        current_ = memory_;
        setStateFlag(PoolStateFlag::scanOtherBlocks, memory_ && memory_->next);
        setOperation(Operation::idle);
    }

    /**
     * @brief 析构所有活动对象并把所有页对齐物理块归还给 allocator。
     *
     * @post 成功完成后 memory_、tail_、current_ 均为 nullptr, 扫描提示位被清除,
     *       池回到可再次 init 或分配的延迟初始化状态。
     *
     * @note 实现不传播 C++ 异常，可以重复调用；为兼容旧接口，公开签名未声明 noexcept。
     *       操作开始时先摘下整条链，析构重入无法接触正在释放的旧块。字节池和平凡
     *       析构对象池为 O(块数)，非平凡对象池还需 O(高水位槽数)。
     */
    void destroy()
    {
        if (operationValue() != Operation::idle)
            return;

        setOperation(Operation::destroying);
        destroyOwnedBlocksDuringProtectedOperation();
        setOperation(Operation::idle);
    }

    /**
     * @brief 所有物理块占用的总字节数, 包含块头、对齐填充和持久位图。
     *
     * @return 块链中 head->size 的总和, 单位: 字节；空池返回 0。
     * @throws std::length_error 总和超过 size_t 可表示范围, 通常表示尺寸极大或元数据损坏。
     * @note 本函数只读遍历全部物理块, 时间复杂度为 O(物理块数量)。
     */
    size_t size() const
    {
        size_t total = 0;
        for (MemoryHead* head = memory_; head; head = head->next)
            total = checkedAdd(total, head->size, "内存池总尺寸溢出");
        return total;
    }

    /**
     * @brief 返回最终对齐后的单槽尺寸。
     * @return 每次分配可用的槽位尺寸, 单位: 字节；该值可能大于构造时请求的原始尺寸。
     * @note 本函数为 noexcept, 时间复杂度为 O(1)。池对象不保存原始请求尺寸。
     */
    size_t get_slot() const noexcept
    {
        return slotSize_;
    }

    /**
     * @brief 返回槽位首地址的对齐要求。
     * @return 槽位对齐值, 单位: 字节；始终是非零的 2 次幂。
     * @note 本函数为 noexcept。完整值不作为 size_t 成员保存, 而是由 alignmentShift_
     *       通过 1 << alignmentShift_ 在 O(1) 时间内无损恢复。
     */
    size_t get_alignment() const noexcept
    {
        return slotAlignmentValue();
    }

    /**
     * @brief 查询底层物理块使用的真实系统页大小。
     * @return 系统页大小, 单位: 字节；系统查询失败时返回 0。
     * @note 本函数为 noexcept, 结果由 memory_pool_system_page_size() 缓存。
     */
    size_t get_page_size() const noexcept
    {
        return memory_pool_system_page_size();
    }

    /**
     * @brief 统计当前拥有的物理块数量。
     * @return 块链节点数量, 单位: 块；延迟初始化的空池返回 0。
     * @note 本函数为 noexcept, 时间复杂度为 O(物理块数量)。
     */
    size_t block_count() const noexcept
    {
        size_t result = 0;
        for (MemoryHead* head = memory_; head; head = head->next)
            ++result;
        return result;
    }

#if defined(MEMORYPOOL_TEST_DIAGNOSTICS)
    /**
     * @brief 返回最近一次 reserveSlot() 实际检查的非热点旧块数量。
     * @return 扫描数量, 单位: 物理块。
     * @note 本函数为 noexcept, 仅用于证明"已知旧块全满"后不会重复 O(B) 扫描。
     *       生产构建不包含此成员和接口；所有翻译单元必须一致定义诊断宏。
     */
    size_t diagnostic_last_scanned_blocks() const noexcept
    {
        return diagnosticLastScanCount_;
    }
#endif

    /**
     * @brief 池是否处于完整空状态。
     *
     * @retval true 块链结构有效, 并且每个块的 bump 指针、free-list、freeCount 和整份
     *              持久位图均处于完整空状态；尚未建立物理块也视为空。
     * @retval false 存在活动槽、元数据不一致, 或池正处于重入保护期。
     * @note 实现不传播 C++ 异常，但保留旧版未声明 noexcept 的公开签名；会先执行完整
     *       validate()，属于 O(块数 + 高水位槽数 + free-list 节点数 + 位图字节数)
     *       的诊断冷路径。
     */
    bool is_empty() const
    {
        if (operationValue() != Operation::idle || !validate())
            return false;
        return allBlocksCompletelyEmpty();
    }

    /**
     * @brief 获取指定物理块的只读布局数据, 供诊断和独立测试使用。
     *
     * @param[in] index 从块链首开始的零基物理块索引, 单位: 块。
     * @param[out] output 成功时写入对应块的地址、字节数和槽位数量快照；失败时保持原值。
     * @retval true index 对应的物理块存在, output 已完整写入。
     * @retval false index 超出当前块链范围。
     * @note 本函数为 noexcept, 时间复杂度为 O(index)。output.bitmapBytes 是从
     *       MemoryHead 尾后地址到 dataBegin 的位图对齐占位跨度, 不是未补齐的原始
     *       位图字节数；位图首地址可由 dataBegin - bitmapBytes 得到。返回的地址只在
     *       池未发生扩缩、merge、split、move、destroy 的期间有效, 调用方不得修改。
     */
    bool get_block_info(size_t index, BlockInfo& output) const noexcept
    {
        MemoryHead* head = memory_;
        while (head && index != 0)
        {
            head = head->next;
            --index;
        }
        if (!head)
            return false;

        output.blockBase = head;
        output.blockSize = head->size;
        output.dataBegin = head->dataBegin;
        output.dataEnd = head->dataEnd;
        output.bitmapBytes = head->stateBytes;
        output.slotCapacity = slotCapacity(head);
        output.highWaterSlots = highWaterSlots(head);
        output.freeSlots = head->freeCount;
        return true;
    }

    /**
     * @brief 完整验证块链、页/槽对齐、位图和 free-list 不变量。
     *
     * 验证内容包括: 空池三指针一致性、块链无环、tail_ 与 current_ 可达性、物理块
     * 页对齐、槽位区边界、位图尺寸、活动位合法性、free-list 节点合法性及数量守恒。
     *
     * @retval true 所有结构和计数不变量均成立。
     * @retval false 池正处于受保护操作, 或发现任意元数据、边界、对齐、链表不变量损坏。
     * @note 本函数为 noexcept, 是 O(块数 + 高水位槽数 + free-list 节点数 + 位图字节数)
     *       的冷路径, 不进入 alloc/free 热路径。
     */
    bool validate() const noexcept
    {
        // 受保护操作可能处于"已清活动位但尚未完成析构"的合法瞬时状态, 不对外验证。
        if (operationValue() != Operation::idle)
            return false;

        // 空池必须同时清空链首、真实链尾和热点指针, 不能只清其中一部分。
        if ((memory_ == nullptr) != (current_ == nullptr))
            return false;
        if ((memory_ == nullptr) != (tail_ == nullptr))
            return false;

        // 先用 Floyd 算法拒绝损坏的块链环, 避免后续诊断自身死循环。
        MemoryHead* slow = memory_;
        MemoryHead* fast = memory_;
        while (fast && fast->next)
        {
            slow = slow->next;
            fast = fast->next->next;
            if (slow == fast)
                return false;
        }

        // 单次遍历验证每块局部布局, 同时证明 current_ 可达并重新计算真实链尾。
        bool foundCurrent = current_ == nullptr;
        MemoryHead* actualTail = nullptr;
        for (MemoryHead* head = memory_; head; head = head->next)
        {
            actualTail = head;
            if (head == current_)
                foundCurrent = true;
            if (!validateBlock(head))
                return false;

            // scan=false 表示除 current_ 外的所有旧块都已被证明无可分配槽；同时复核提示位。
            if (!hasStateFlag(PoolStateFlag::scanOtherBlocks) && head != current_)
            {
                const std::uintptr_t item = reinterpret_cast<std::uintptr_t>(head->item);
                const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(head->dataEnd);
                if (head->freeList != nullptr
                    || (end >= item && static_cast<size_t>(end - item) >= slotSize_))
                    return false;
            }
        }
        return foundCurrent && actualTail == tail_ && (!tail_ || tail_->next == nullptr);
    }

    /**
     * @brief 输出每块的布局和活动状态。
     *
     * 每行包含块序号、物理地址、总字节数、槽位区范围、高水位槽数量、free-list
     * 节点数量和位图对齐占位字节数, 最后输出所有块的 free-list 节点总数。
     *
     * @note 实现不传播 C++ 异常，但保留旧版未声明 noexcept 的公开签名。输出目标为
     *       stdout，仅供人工诊断；不验证元数据，也不保证并发一致快照。
     */
    void dump() const
    {
        size_t index = 0;
        size_t totalFree = 0;
        for (MemoryHead* head = memory_; head; head = head->next)
        {
            const size_t highWater = highWaterSlots(head);
            totalFree += head->freeCount;
            std::printf("%03zu: 块=%p, 总尺寸=%zu, data=[%p,%p), 高水位=%zu, free-list=%zu, 位图占位=%zu\n",
                        index++, static_cast<void*>(head), head->size,
                        static_cast<void*>(head->dataBegin), static_cast<void*>(head->dataEnd),
                        highWater, head->freeCount, head->stateBytes);
        }
        std::printf("总空闲节点数: %zu\n", totalFree);
    }

protected:
    /**
     * @brief 具体对象池在释放单个活动槽前调用的析构 hook。
     *
     * @param[in] value 即将归还的活动槽位首地址。Base 调用本方法前已经清除对应活动位。
     * @note 默认实现不执行操作, 供只管理原始字节的池使用。本函数为 noexcept；派生类
     *       覆盖时也不得传播异常。先清活动位可防止析构函数重入后再次析构同一对象。
     */
    virtual void beforeFree(void* value) noexcept
    {
        (void)value;
    }

    /**
     * @brief 析构块中全部活动对象。
     *
     * @param[in,out] head 待集中清理的物理块头。派生类只负责活动对象的析构, 不负责
     *                     释放物理块、重置 bump 指针、free-list 或整份位图。
     * @note 默认实现不执行操作, 供字节池使用。本函数为 noexcept；对象池覆盖实现会
     *       直接遍历持久活动位图, 并在调用用户析构函数前逐个清除活动位。
     */
    virtual void destroyBlock(MemoryHead* head) noexcept
    {
        (void)head;
    }

    /**
     * @brief 字节池等"释放时不运行任何用户代码"场景使用的空释放 hook。
     * @note invokes_user_code 为 false_type 时, freeWithHook() 在编译期裁掉
     *       releasing/idle 状态切换：没有用户代码就不存在同池析构重入的可能。
     */
    struct TrivialFreeHook
    {
        using invokes_user_code = std::false_type; // 释放路径不执行任何用户代码。

        /** @brief 空操作；槽位内容无需析构。 */
        void operator()(void* value) const noexcept
        {
            (void)value;
        }
    };

    /**
     * @brief Base 公开 free(void*) 使用的虚派发 hook。
     * @note 保持旧行为：直接以 Base 引用释放, 或外部派生类覆盖 beforeFree() 时,
     *       仍经过虚函数到达派生析构逻辑。无法静态证明没有用户代码, 因此保守
     *       声明 invokes_user_code 为 true_type, 保留 releasing 重入保护。
     */
    struct VirtualFreeHook
    {
        using invokes_user_code = std::true_type; // 可能进入派生类用户代码。

        CMemoryPoolBase* pool; // 发起释放的池, 用于虚派发 beforeFree()。

        /** @brief 通过虚函数调用派生类的单对象析构 hook。 */
        void operator()(void* value) const noexcept
        {
            pool->beforeFree(value);
        }
    };

    /**
     * @brief 归还槽位的公共实现, 释放动作由编译期确定的 hook 提供。
     *
     * 与旧版公开 free(void*) 流程完全一致, 仅把"每次释放一次虚函数调用"替换为
     * 静态 hook：具体池在自身 free() 中传入直连析构函数的 hook, 平凡析构类型的
     * hook 是空操作且整个 releasing 状态切换都会被编译期裁掉。
     *
     * @tparam Hook 释放 hook 类型；必须提供 noexcept 的 operator()(void*) 和
     *              invokes_user_code 布尔常量类型。
     * @param[in] pointer 待归还的活动槽位首地址；允许为 nullptr。
     * @param[in] hook 完成对象析构(或空操作)的可调用对象。
     * @retval true 活动位已清除、hook 已执行、槽位已归还。
     * @retval false pointer 为空、来自外部、不是槽首、已释放, 或发生同池重入。
     * @note 本函数不传播 C++ 异常。验证顺序、热点提升和扫描提示与公开 free() 文档一致。
     */
    template<class Hook>
    bool freeWithHook(void* pointer, const Hook& hook)
    {
        // 入口仍然拒绝空指针、空池和一切受保护操作期间的重入。
        if (!pointer || !current_ || operationValue() != Operation::idle)
            return false;

        const BytePointer value = reinterpret_cast<BytePointer>(pointer);
        if (tryFreeBlock(current_, value, hook))
        {
            // 释放会重新制造可分配空间；后续当前块耗尽时允许再检查其他块。
            // 先读后写：提示位通常已经置位, 避免每次释放都无条件弄脏池对象缓存行。
            if (!hasStateFlag(PoolStateFlag::scanOtherBlocks))
                setStateFlag(PoolStateFlag::scanOtherBlocks, true);
            return true;
        }

        // 若地址落在当前物理块内, 它不可能同时属于链上的另一个独立物理块。
        // 因而这里可以安全省去后续 O(B) 的块链遍历。
        if (containsPhysicalAddress(current_, value))
            return false;

        for (MemoryHead* head = memory_; head; head = head->next)
        {
            if (head != current_ && tryFreeBlock(head, value, hook))
            {
                // 批量释放旧块时把命中的块提升为热点, 后续同块释放由 O(B) 降为 O(1)。
                current_ = head;
                setStateFlag(PoolStateFlag::scanOtherBlocks, true);
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 对象池开始一次"预留槽位但尚未构造"的事务。
     *
     * 成功返回后 operation 状态保持 constructing, 槽位尚未写入活动位；派生对象池必须
     * 随后调用 commitObjectConstruction(), 或在对象构造抛异常时调用
     * rollbackObjectConstruction(), 两者恰好选择一个。
     *
     * @return 包含槽位地址、来源块和回滚快照的预留记录。
     * @throws std::logic_error 当前池不是 idle, 或发现 free-list 与 freeCount 不一致。
     * @throws std::length_error 尺寸运算溢出或新物理块超过 allocator 上限。
     * @throws std::runtime_error 无法取得有效的系统页大小。
     * @throws std::bad_alloc 新物理块分配失败或页对齐契约不成立。
     * @note 自定义 Allocator::allocate 若抛出其他异常, 该异常原样传播。
     */
    RawReservation beginObjectConstruction()
    {
        requireIdle("对象构造期间发生内存池重入");
        setOperation(Operation::constructing);
        try
        {
            // reserveSlot 只预留 raw storage；用户对象此时尚未开始生命周期。
            return reserveSlot();
        }
        catch (...)
        {
            // reserveSlot 若失败会保持块链自身可析构；必须解除重入保护后再原样传播。
            setOperation(Operation::idle);
            throw;
        }
    }

    /**
     * @brief 提交一次已成功完成用户对象构造的槽位预留事务。
     *
     * @param[in] reservation beginObjectConstruction() 返回且尚未提交/回滚的记录。
     * @post 对应活动位设为 1, operation 恢复 idle, 对象可被 query/free/clear 识别。
     * @note 本函数为 noexcept。Debug 构建会断言当前 operation 必须为 constructing；
     *       传入不匹配、过期或重复使用的记录属于调用方违反内部前置条件。
     */
    void commitObjectConstruction(const RawReservation& reservation) noexcept
    {
        assert(operationValue() == Operation::constructing);
        const size_t offset =
            static_cast<size_t>(reservation.address - reservation.head->dataBegin);
        // reservation 只可能来自 bump、free-list 或刚建立的新块，边界已经在预留阶段证明。
        // 直接按乘法逆换算的槽索引提交，不执行硬件除法，也不再进入完整边界检查。
        setStateIndexInUse(reservation.head, slotIndexOfOffset(offset), true);
        setOperation(Operation::idle);
    }

    /**
     * @brief 在用户对象构造抛异常后精确撤销尚未提交的槽位预留。
     *
     * @param[in] reservation beginObjectConstruction() 返回且尚未提交/回滚的记录。
     * @post bump 来源回退高水位；free-list 来源重新链回原槽；newBlock 来源摘除并释放
     *       本次新块。current_ 和旧块扫描提示恢复为事务应有状态, operation 恢复 idle。
     * @note 本函数为 noexcept, 不调用用户对象析构函数, 因为对象构造未成功完成。
     */
    void rollbackObjectConstruction(const RawReservation& reservation) noexcept
    {
        assert(operationValue() == Operation::constructing);

        if (reservation.origin == ReservationOrigin::newBlock)
        {
            // 新块尚无已提交对象, 可以从链尾直接摘除并整体归还 allocator。
            if (reservation.previousTail)
                reservation.previousTail->next = nullptr;
            else
                memory_ = nullptr;
            tail_ = reservation.previousTail;
            current_ = reservation.previousCurrent;
            releaseBlockMemory(reservation.head);
            // reserveSlot 已在扩块前证明全部旧块不可分配；构造失败不改变这个结论。
            setStateFlag(PoolStateFlag::scanOtherBlocks, false);
        }
        else if (reservation.origin == ReservationOrigin::bump)
        {
            // bump 预留只前移过一次 item, 按原地址回退即可恢复连续未使用区。
            assert(reservation.head->item == reservation.address + slotSize_);
            reservation.head->item = reservation.address;
            current_ = reservation.previousCurrent;
            setStateFlag(PoolStateFlag::scanOtherBlocks,
                         reservation.previousScanFlags != 0);
        }
        else
        {
            // free-list 预留已摘掉一个节点；在原槽位存储中重建节点并重新压回表头。
            ListNode* node = ::new (static_cast<void*>(reservation.address)) ListNode;
            node->next = reservation.head->freeList;
            reservation.head->freeList = node;
            ++reservation.head->freeCount;
            current_ = reservation.previousCurrent;
            setStateFlag(PoolStateFlag::scanOtherBlocks,
                         reservation.previousScanFlags != 0);
        }

        setOperation(Operation::idle);
    }

    /**
     * @brief 字节池使用的已提交裸槽分配。
     *
     * @return 已标记为活动的原始槽位首地址；可用空间为 get_slot() 字节, 地址满足
     *         get_alignment()。本函数不构造对象, 也不清零用户槽位内容。
     * @throws std::logic_error 当前池处于受保护操作, 或内部 free-list 计数损坏。
     * @throws std::length_error 尺寸运算溢出或新物理块超过 allocator 上限。
     * @throws std::runtime_error 无法取得有效的系统页大小。
     * @throws std::bad_alloc 新物理块分配失败或页对齐契约不成立。
     * @note 自定义 Allocator::allocate 若抛出其他异常, 该异常原样传播。快路径
     *       (热点块 bump/free-list 命中)不全程进入 managing 状态：中间没有任何
     *       用户代码, 不存在重入窗口。只有扩块调用 allocator 时由 reserveSlot 内部
     *       短暂进入 managing：allocate 回调期间重入同池 malloc 抛 std::logic_error,
     *       重入 free 返回 false, 该契约由 allocator_callbacks 测试锁定。
     */
    void* alloc()
    {
        // 入口重入检查保留：clear/destroy/析构等保护期内分配仍然抛 logic_error。
        requireIdle("内存池分配期间发生重入");
        // 字节池没有可能抛异常的用户构造阶段, 因此预留后可以立即提交活动位。
        RawReservation reservation = reserveSlot();
        const size_t offset =
            static_cast<size_t>(reservation.address - reservation.head->dataBegin);
        // 槽索引用乘法逆精确除法换算, 提交活动位的热路径不含硬件除法。
        setStateIndexInUse(reservation.head, slotIndexOfOffset(offset), true);
        return reservation.address;
    }

    /**
     * @brief 在完整空池上修改运行时槽位尺寸, 并移除无法容纳新槽的旧物理块。
     *
     * @param[in] requestedSize 新的原始单槽请求尺寸, 单位: 字节；最终值至少为
     *                          sizeof(ListNode), 并按原有槽位对齐要求向上取整。
     * @throws std::invalid_argument requestedSize 为 0。
     * @throws std::length_error requestedSize 向上对齐溢出或最终尺寸超过 UINT32_MAX。
     * @throws std::logic_error 池正处于受保护操作、元数据校验失败, 或仍有活动槽位。
     *
     * @post 保留下来的完整空块按新槽尺寸重新解释；容量为 0 的空块被归还。函数不会
     *       主动建立新物理块, 也不会改变槽位地址对齐要求。
     * @note 复杂度包含完整 validate 和新位图清零，为 O(块数 + 高水位槽数 +
     *       free-list 节点数 + 旧/新位图字节数)。
     */
    void resizeSlot(size_t requestedSize)
    {
        requireIdle("清理期间不能改变槽位尺寸");
        const uint32_t newSize = alignSlot(requestedSize, slotAlignmentValue());
        if (!validate())
            throw std::logic_error("resize_slot: 内存池块链或元数据校验失败");
        if (!allBlocksCompletelyEmpty())
            throw std::logic_error("resize_slot: 池中仍有活动槽位或元数据不完整");

        ManagingOperationScope operation(*this);

        // 先从公开可见状态摘下旧链，避免 deallocate 回调沿成员指针访问已析构块头。
        MemoryHead* detached = memory_;
        memory_ = nullptr;
        tail_ = nullptr;
        current_ = nullptr;
        setStateFlag(PoolStateFlag::scanOtherBlocks, false);

        // 只有完整空池才能安全改变槽跨度。提交新尺寸后，每个块都按新尺寸重新计算
        // 候选槽数、位图字节数、dataBegin 和最终容量；不能沿用旧槽尺寸留下的边界。
        slotSize_ = newSize;
        // 槽尺寸变化后, 热路径快速除法常量必须同步重建。
        updateSlotDivisionConstants();
        MemoryHead* rebuiltTail = nullptr;
        memory_ = rebuildEmptyChainForCurrentSlot(detached, rebuiltTail);
        tail_ = rebuiltTail;
        current_ = memory_;
        setStateFlag(PoolStateFlag::scanOtherBlocks, memory_ && memory_->next);
    }

    /**
     * @brief 接管来源池的全部完整空块, 并按当前池槽尺寸重新解释这些块。
     *
     * @param[in,out] other 来源池；必须整体为空。成功后来源池不再拥有任何物理块。
     * @throws std::logic_error 任一池处于受保护操作、元数据损坏, 或来源池并非完整空池。
     * @throws std::invalid_argument 两池对齐要求或 allocator owner 不兼容。
     *
     * @note 两池槽尺寸可以不同, 但槽位地址对齐必须相同。自合并直接返回。追加后会
     *       释放在当前槽尺寸下容量为 0 的块；默认保持块链原顺序。有状态 allocator
     *       的相等比较若抛出其他异常, 该异常会在所有权转移前原样传播。默认复杂度为
     *       O(块数 + 高水位槽数 + free-list 节点数 + 旧/新位图字节数)；启用
     *       MEMORYPOOL_SORT_AFTER_MERGE_SPLIT 后还可能叠加 O(块数平方) 排序。
     */
    void mergePool(CMemoryPoolBase& other)
    {
        if (&other == this)
            return;
        requireIdle("merge: 当前池正在执行其他操作");
        other.requireIdle("merge: 来源池正在执行其他操作");
        if (alignmentShift_ != other.alignmentShift_)
            throw std::invalid_argument("merge: 两个池的槽位对齐要求不兼容");
        {
            ManagingOperationScope currentOperation(*this);
            ManagingOperationScope otherOperation(other);
            if (!allocatorsCompatible(other))
                throw std::invalid_argument("merge: 两个池的 allocator 不兼容");
        }
        if (!validate() || !other.validate())
            throw std::logic_error("merge: 内存池块链或元数据校验失败");
        if (!other.allBlocksCompletelyEmpty())
            throw std::logic_error("merge: 来源池仍有活动槽位或元数据不完整");

        ManagingOperationScope currentOperation(*this);
        ManagingOperationScope otherOperation(other);

        // 校验全部完成以后才转移所有权, 避免异常把链留在两个池之间。
        MemoryHead* detached = other.memory_;
        other.memory_ = nullptr;
        other.tail_ = nullptr;
        other.current_ = nullptr;
        other.setStateFlag(PoolStateFlag::scanOtherBlocks, false);
        // 来源块完整为空，因此可以丢弃旧槽编号，并按目标池槽尺寸重建块内布局。
        MemoryHead* rebuiltTail = nullptr;
        detached = rebuildEmptyChainForCurrentSlot(detached, rebuiltTail);
        appendBlocks(detached, rebuiltTail);
    }

    /**
     * @brief 摘取源链上的全部完整空块并追加到 target。
     *
     * @param[in,out] target 目标池；可以已经拥有物理块。成功摘取的块从当前池转移给它。
     * @return 从来源链摘下的完整空物理块数量, 单位: 块；自分离或空来源返回 0。
     * @throws std::logic_error 任一池处于受保护操作, 或任一池元数据校验失败。
     * @throws std::invalid_argument 两池对齐要求或 allocator owner 不兼容。
     *
     * @note 当前池可以仍有活动槽；只有 bump、free-list、计数和位图全部为空的完整块
     *       才会被摘取。两池槽尺寸可以不同；目标尺寸下容量为 0 的摘取块会被立即归还，
     *       因此返回值可能大于 target 最终增加的块数。复杂度包含双方完整校验、空块
     *       识别和新位图清零，为 O(块数 + 高水位槽数 + free-list 节点数 + 旧/新位图
     *       字节数)；启用 MEMORYPOOL_SORT_AFTER_MERGE_SPLIT 后还可能叠加 O(块数平方)
     *       排序。有状态 allocator 的相等比较异常会在修改任一块链前原样传播。
     */
    size_t splitPool(CMemoryPoolBase& target)
    {
        if (&target == this || !memory_)
            return 0;
        requireIdle("split: 来源池正在执行其他操作");
        target.requireIdle("split: 目标池正在执行其他操作");
        if (alignmentShift_ != target.alignmentShift_)
            throw std::invalid_argument("split: 两个池的槽位对齐要求不兼容");
        {
            ManagingOperationScope sourceOperation(*this);
            ManagingOperationScope targetOperation(target);
            if (!allocatorsCompatible(target))
                throw std::invalid_argument("split: 两个池的 allocator 不兼容");
        }
        if (!validate() || !target.validate())
            throw std::logic_error("split: 内存池块链或元数据校验失败");

        ManagingOperationScope sourceOperation(*this);
        ManagingOperationScope targetOperation(target);

        MemoryHead* detachedHead = nullptr;
        MemoryHead* detachedTail = nullptr;
        MemoryHead* previous = nullptr;
        MemoryHead* head = memory_;
        bool currentDetached = false;
        size_t detachedCount = 0;

        // 单次遍历同时维护来源链和待转移链, 能够摘除链首、中间及链尾的全部空块。
        while (head)
        {
            MemoryHead* next = head->next;
            if (isBlockCompletelyEmpty(head))
            {
                if (previous)
                    previous->next = next;
                else
                    memory_ = next;

                head->next = nullptr;
                if (detachedTail)
                    detachedTail->next = head;
                else
                    detachedHead = head;
                detachedTail = head;

                currentDetached = currentDetached || head == current_;
                ++detachedCount;
            }
            else
            {
                previous = head;
            }
            head = next;
        }

        // 重建来源池热点、真实链尾和扫描提示, 再把完整的 detached 链交给目标池。
        if (currentDetached)
            current_ = memory_;
        tail_ = previous;
        setStateFlag(PoolStateFlag::scanOtherBlocks, memory_ && memory_->next);
        // 摘下来的每个块都完整为空；接入目标前必须按目标槽尺寸重新生成位图布局。
        MemoryHead* rebuiltTail = nullptr;
        detachedHead = target.rebuildEmptyChainForCurrentSlot(detachedHead, rebuiltTail);
        target.appendBlocks(detachedHead, rebuiltTail);
        return detachedCount;
    }

    /**
     * @brief 交换两个池的 allocator、槽布局、块链、热点和组合状态。
     *
     * @param[in,out] other 与当前池交换全部所有权状态的另一个池；允许自交换。
     * @throws std::logic_error 任一池处于受保护操作。
     * @throws std::invalid_argument allocator 不传播且两个实例不兼容。
     *
     * @note 自交换直接返回。有状态 allocator 的相等比较若抛异常, 会在交换前原样传播；
     *       传播型 allocator 的 swap 被编译期约束为 noexcept。诊断扫描计数不属于池语义,
     *       交换后两侧均重置为 0。
     */
    void swapPool(CMemoryPoolBase& other)
    {
        if (&other == this)
            return;
        requireIdle("swap: 当前池正在执行其他操作");
        other.requireIdle("swap: 对方池正在执行其他操作");

        ManagingOperationScope currentOperation(*this);
        ManagingOperationScope otherOperation(other);

        // 先完成所有可能失败的兼容性检查, 再交换任何块链状态。
        using Propagate = typename AllocatorTraits::propagate_on_container_swap;
        prepareAllocatorSwap(other, Propagate());
        swapAllocator(other, Propagate());

        using std::swap;
        swap(slotSize_, other.slotSize_);
        // 快速除法常量由 slotSize_ 唯一决定, 必须与其一起交换。
        swap(slotDivisorShift_, other.slotDivisorShift_);
        swap(slotDivisorInverse_, other.slotDivisorInverse_);
        swap(slotDivisorLimit_, other.slotDivisorLimit_);
        swap(memory_, other.memory_);
        swap(tail_, other.tail_);
        swap(current_, other.current_);
        swap(alignmentShift_, other.alignmentShift_);
        swap(stateFlags_, other.stateFlags_);
#if defined(MEMORYPOOL_TEST_DIAGNOSTICS)
        diagnosticLastScanCount_ = 0;
        other.diagnosticLastScanCount_ = 0;
#endif
    }

    /**
     * @brief 销毁当前池原有内容后, 移动接管来源池的全部所有权状态。
     *
     * @param[in,out] other 来源池；成功后其块链为空、状态恢复 idle；始终可析构或重新赋值。
     *                      默认无状态 allocator 下可直接重新使用；自定义 allocator 能否
     *                      再次分配取决于其移动后状态契约。
     * @throws std::logic_error 任一池处于受保护操作。
     * @throws std::invalid_argument allocator 不传播且两个实例不兼容。
     *
     * @note 自移动赋值直接返回。有状态 allocator 的相等比较异常会在销毁目标旧内容前
     *       原样传播；提交阶段的 destroy 和传播型 allocator 移动赋值均被保证 noexcept,
     *       因此对所有可抛检查提供强异常保证。
     */
    void moveAssignPool(CMemoryPoolBase&& other)
    {
        if (&other == this)
            return;
        requireIdle("move: 当前池正在执行其他操作");
        other.requireIdle("move: 来源池正在执行其他操作");

        ManagingOperationScope currentOperation(*this);
        ManagingOperationScope otherOperation(other);

        // 所有可能失败的兼容性检查必须先于 destroy(), 避免无谓丢失目标池旧内容。
        using Propagate = typename AllocatorTraits::propagate_on_container_move_assignment;
        prepareAllocatorMove(other, Propagate());
        destroyOwnedBlocksDuringProtectedOperation();
        moveAssignAllocator(other, Propagate());

        const bool scanOtherBlocks = other.hasStateFlag(PoolStateFlag::scanOtherBlocks);
        slotSize_ = other.slotSize_;
        // 快速除法常量随 slotSize_ 一起接管, 保持"由槽尺寸唯一决定"的不变量。
        slotDivisorShift_ = other.slotDivisorShift_;
        slotDivisorInverse_ = other.slotDivisorInverse_;
        slotDivisorLimit_ = other.slotDivisorLimit_;
        memory_ = other.memory_;
        tail_ = other.tail_;
        current_ = other.current_;
        alignmentShift_ = other.alignmentShift_;
        setStateFlag(PoolStateFlag::scanOtherBlocks, scanOtherBlocks);
        other.memory_ = nullptr;
        other.tail_ = nullptr;
        other.current_ = nullptr;
        other.setStateFlag(PoolStateFlag::scanOtherBlocks, false);
#if defined(MEMORYPOOL_TEST_DIAGNOSTICS)
        diagnosticLastScanCount_ = 0;
        other.diagnosticLastScanCount_ = 0;
#endif
    }

    /**
     * @brief 供派生对象池读取指定槽位的持久活动位。
     * @param[in] head 槽位所属物理块头。
     * @param[in] slotOffset 槽位相对 dataBegin 的字节偏移, 单位: 字节。
     * @retval true 对应活动位为 1。
     * @retval false 对应活动位为 0, 或计算出的位图索引越界。
     * @note 本函数为 noexcept；调用方应传入按 slotSize_ 对齐的合法槽偏移。内部还会
     *       验证偏移确实对应数据区中的一个完整槽，保证尾部余量不会映射到位图补齐区。
     */
    bool slotInUse(MemoryHead* head, size_t slotOffset) const noexcept
    {
        return isSlotInUse(head, slotOffset);
    }

    /**
     * @brief 供派生对象池按零基槽索引读取活动位, 避免整块析构循环重复执行除法。
     * @param[in] head 槽位所属物理块头。
     * @param[in] slotIndex 槽位相对 dataBegin 的零基索引, 单位: 槽。
     * @return 对应活动位是否为 1；调用方必须保证索引小于高水位槽数。
     */
    bool slotIndexInUse(MemoryHead* head, size_t slotIndex) const noexcept
    {
        return isStateIndexInUse(head, slotIndex);
    }

    /**
     * @brief 供派生对象池修改指定槽位的持久活动位。
     * @param[in,out] head 槽位所属物理块头。
     * @param[in] slotOffset 槽位相对 dataBegin 的字节偏移, 单位: 字节。
     * @param[in] active true 表示活动, false 表示非活动。
     * @note 本函数为 noexcept。Debug 构建断言偏移和位图索引有效；Release 越界时不写入。
     */
    void setSlotActive(MemoryHead* head, size_t slotOffset, bool active) noexcept
    {
        setSlotInUse(head, slotOffset, active);
    }

    /**
     * @brief 供派生对象池按零基槽索引修改活动位, 避免整块析构循环重复执行除法。
     * @param[in,out] head 槽位所属物理块头。
     * @param[in] slotIndex 槽位相对 dataBegin 的零基索引, 单位: 槽。
     * @param[in] active true 置位, false 清位。
     */
    void setSlotIndexActive(MemoryHead* head, size_t slotIndex, bool active) noexcept
    {
        setStateIndexInUse(head, slotIndex, active);
    }

    /**
     * @brief 供派生对象池取得最终对齐后的槽位跨度。
     * @return 单槽跨度, 单位: 字节。
     * @note 本函数为 noexcept, 时间复杂度为 O(1)。
     */
    size_t slotSizeValue() const noexcept
    {
        return slotSize_;
    }

private:
    /** @brief 在调用 allocator 或重组块链期间拒绝同池修改重入。 */
    class ManagingOperationScope final
    {
    public:
        explicit ManagingOperationScope(CMemoryPoolBase& pool) noexcept
            : pool_(pool)
        {
            assert(pool_.operationValue() == Operation::idle);
            pool_.setOperation(Operation::managing);
        }

        ~ManagingOperationScope() noexcept
        {
            pool_.setOperation(Operation::idle);
        }

        ManagingOperationScope(const ManagingOperationScope&) = delete;
        ManagingOperationScope& operator=(const ManagingOperationScope&) = delete;

    private:
        CMemoryPoolBase& pool_;
    };

    /**
     * @brief 仅在池当前处于 idle 时才进入 managing 的条件保护作用域。
     *
     * reserveSlot() 的扩块路径要调用 allocator 用户代码, 必须拒绝回调重入；但该
     * 路径既服务于字节池快路径(进入时为 idle), 也服务于对象池构造事务(进入时已是
     * constructing, 本身已经拒绝重入)。本作用域只在前者补上 managing, 后者保持
     * 原状态不变, 避免破坏 constructing 语义。
     */
    class ConditionalManagingScope final
    {
    public:
        /** @brief 池处于 idle 时切换到 managing；否则不改变现有保护状态。 */
        explicit ConditionalManagingScope(CMemoryPoolBase& pool) noexcept
            : pool_(pool)
            , engaged_(pool.operationValue() == Operation::idle)
        {
            if (engaged_)
                pool_.setOperation(Operation::managing);
        }

        /** @brief 仅当本作用域自己进入了 managing 时才恢复 idle。 */
        ~ConditionalManagingScope() noexcept
        {
            if (engaged_)
                pool_.setOperation(Operation::idle);
        }

        ConditionalManagingScope(const ConditionalManagingScope&) = delete;
        ConditionalManagingScope& operator=(const ConditionalManagingScope&) = delete;

    private:
        CMemoryPoolBase& pool_; // 被保护的池。
        bool engaged_;          // 本作用域是否真正执行了 idle -> managing 切换。
    };

    /**
     * @brief 在调用方已经设置保护状态时摘链并销毁当前池拥有的全部物理块。
     * @note 本函数不修改 Operation；调用方负责在整个 allocator 回调期间保持保护状态。
     */
    void destroyOwnedBlocksDuringProtectedOperation() noexcept
    {
        assert(operationValue() != Operation::idle);
        MemoryHead* head = memory_;
        memory_ = nullptr;
        tail_ = nullptr;
        current_ = nullptr;
        setStateFlag(PoolStateFlag::scanOtherBlocks, false);

        while (head)
        {
            MemoryHead* next = head->next;
            destroyBlock(head);
            releaseBlockMemory(head);
            head = next;
        }
    }

    // C++14 的 allocator_traits 可能没有 is_always_equal；依次尊重 traits 特化、
    // allocator 自身声明，最后按标准规则退回到“空 allocator 可互换”。
    template<class Traits, class Candidate>
    static typename Traits::is_always_equal allocatorAlwaysEqualProbe(int) noexcept;

    template<class Traits, class Candidate>
    static typename Candidate::is_always_equal allocatorAlwaysEqualProbe(long) noexcept;

    template<class Traits, class Candidate>
    static std::is_empty<Candidate> allocatorAlwaysEqualProbe(...) noexcept;

    /**
     * @brief 对两个 size_t 值执行带溢出检查的加法。
     * @param[in] left 左操作数, 单位与调用场景一致。
     * @param[in] right 右操作数, 单位必须与 left 相同。
     * @param[in] message 溢出时写入 std::length_error 的稳定错误消息；必须指向有效字符串。
     * @return left + right, 单位与两个操作数相同。
     * @throws std::length_error 数学结果超过 size_t 可表示范围。
     */
    static size_t checkedAdd(size_t left, size_t right, const char* message)
    {
        if (right > (std::numeric_limits<size_t>::max)() - left)
            throw std::length_error(message);
        return left + right;
    }

    /**
     * @brief 对两个 size_t 值执行带溢出检查的乘法。
     * @param[in] left 左操作数；常用于数量。
     * @param[in] right 右操作数；常用于单项字节数。
     * @param[in] message 溢出时写入 std::length_error 的错误消息。
     * @return left * right；当参数分别为数量和单项字节数时, 单位为字节。
     * @throws std::length_error 数学结果超过 size_t 可表示范围。
     */
    static size_t checkedMultiply(size_t left, size_t right, const char* message)
    {
        if (left != 0 && right > (std::numeric_limits<size_t>::max)() / left)
            throw std::length_error(message);
        return left * right;
    }

    /**
     * @brief 将无符号尺寸向上取整到指定粒度的整数倍。
     * @param[in] value 待取整值, 单位: 字节或调用方使用的同类计数单位。
     * @param[in] alignment 取整粒度, 单位与 value 相同；必须大于 0, 不要求是 2 次幂。
     * @param[in] message 向上补齐发生溢出时使用的 std::length_error 消息。
     * @return 不小于 value 的最小 alignment 整数倍。
     * @throws std::invalid_argument alignment 为 0。
     * @throws std::length_error value 加上补齐量时超过 size_t 范围。
     */
    static size_t checkedAlignUp(size_t value, size_t alignment, const char* message)
    {
        if (alignment == 0)
            throw std::invalid_argument("对齐值不能为 0");
        const size_t remainder = value % alignment;
        return remainder == 0 ? value : checkedAdd(value, alignment - remainder, message);
    }

    /**
     * @brief 判断无符号整数是否为非零的 2 次幂。
     * @param[in] value 待判断数值。
     * @retval true value 恰好只有一个二进制位为 1。
     * @retval false value 为 0 或包含多个置位。
     * @note 本函数为 noexcept, 时间复杂度为 O(1)。
     */
    static bool isPowerOfTwo(size_t value) noexcept
    {
        return value != 0 && (value & (value - 1)) == 0;
    }

    /**
     * @brief 校验并规范化调用方请求的槽位地址对齐值。
     * @param[in] requested 请求对齐, 单位: 字节；必须是非零的 2 次幂。
     * @return max(requested, alignof(ListNode)), 单位: 字节。
     * @throws std::invalid_argument requested 为 0 或不是 2 次幂。
     * @note 最小提升保证任一释放槽都能安全原地存放 ListNode。
     */
    static size_t normaliseAlignment(size_t requested)
    {
        if (!isPowerOfTwo(requested))
            throw std::invalid_argument("槽位对齐值必须是非零的 2 次幂");
        const size_t nodeAlignment = alignof(ListNode);
        return requested < nodeAlignment ? nodeAlignment : requested;
    }

    /**
     * @brief 把 2 次幂对齐值编码为其以 2 为底的指数。
     * @param[in] alignment 已规范化的对齐值, 单位: 字节；前置条件是非零 2 次幂。
     * @return log2(alignment), 单位: 位移位数；x86 最大 31, x64 最大 63。
     * @note 本函数为 noexcept。uint8_t 足以保存所有 size_t 平台的有效结果。
     */
    static uint8_t alignmentShift(size_t alignment) noexcept
    {
        uint8_t shift = 0;
        while ((size_t(1) << shift) != alignment)
            ++shift;
        return shift;
    }

    /**
     * @brief 从紧凑指数成员恢复完整槽位对齐值。
     * @return size_t(1) << alignmentShift_, 单位: 字节。
     * @note 本函数为 noexcept。构造函数保证 alignmentShift_ 小于 size_t 的有效位数。
     */
    size_t slotAlignmentValue() const noexcept
    {
        return size_t(1) << alignmentShift_;
    }

    /**
     * @brief 对 size_t 宽度的无符号整数执行循环右移。
     * @param[in] value 待旋转值。
     * @param[in] shift 右移位数；必须小于 size_t 的位宽。
     * @return value 循环右移 shift 位的结果。
     * @note 本函数为 noexcept。shift 为 0 时左移位数按位宽掩码折算为 0, 两个分支
     *       都是合法移位, 不产生未定义行为。
     */
    static size_t rotateRight(size_t value, unsigned shift) noexcept
    {
        constexpr unsigned bits = static_cast<unsigned>(std::numeric_limits<size_t>::digits);
        return (value >> shift) | (value << ((bits - shift) & (bits - 1)));
    }

    /**
     * @brief 按当前 slotSize_ 重新生成热路径快速除法常量。
     *
     * 记 slotSize_ = odd * 2^shift（odd 为奇数）。本函数计算并缓存：
     * - slotDivisorShift_：因子 2 的个数 shift；
     * - slotDivisorInverse_：odd 在模 2^N 下的乘法逆（N 为 size_t 位宽）；
     * - slotDivisorLimit_：SIZE_MAX / slotSize_，即合法商的最大值。
     *
     * 此后任意 slotSize_ 的倍数 offset 都满足
     * `offset / slotSize_ == rotateRight(offset * inverse, shift)`（Granlund–Montgomery
     * 精确除法），而非倍数经同一运算得到的值必然大于 slotDivisorLimit_，可同时充当
     * 整除判定。热路径由此把一次约 20～40 周期的硬件除法换成一次乘法加一次旋转。
     *
     * @pre slotSize_ 已经写入最终对齐值且非 0。
     * @note 本函数为 noexcept。本体包含一次设置期硬件除法（求 limit），只在构造、
     *       resize_slot 等冷路径执行；乘法逆用 5 轮牛顿迭代求得, 每轮正确位数翻倍。
     */
    void updateSlotDivisionConstants() noexcept
    {
        assert(slotSize_ != 0);
        // 逐位取出因子 2 的个数；slotSize_ 非 0 保证循环终止。
        unsigned shift = 0;
        while (((slotSize_ >> shift) & 1u) == 0)
            ++shift;
        slotDivisorShift_ = static_cast<uint8_t>(shift);

        // 牛顿迭代求奇数部分的模 2^N 乘法逆：种子 odd 自身有 3 个正确低位,
        // 每轮 inverse *= 2 - odd * inverse 让正确位数翻倍, 5 轮后覆盖 64 位。
        const size_t odd = static_cast<size_t>(slotSize_) >> shift;
        size_t inverse = odd;
        inverse *= size_t(2) - odd * inverse;
        inverse *= size_t(2) - odd * inverse;
        inverse *= size_t(2) - odd * inverse;
        inverse *= size_t(2) - odd * inverse;
        inverse *= size_t(2) - odd * inverse;
        assert(static_cast<size_t>(odd * inverse) == 1);
        slotDivisorInverse_ = inverse;

        // 商上界只在这里做一次真正的除法；热路径此后只用乘法和比较。
        slotDivisorLimit_ = (std::numeric_limits<size_t>::max)() / slotSize_;
    }

    /**
     * @brief 已知 slotOffset 是 slotSize_ 整数倍时, 无除法换算零基槽索引。
     * @param[in] slotOffset 槽首相对 dataBegin 的字节偏移；必须是 slotSize_ 的整数倍。
     * @return slotOffset / slotSize_, 单位: 槽索引。
     * @note 本函数为 noexcept, 由一次乘法和一次循环右移完成。Debug 构建断言商
     *       乘回 slotSize_ 恰好还原 slotOffset, 捕获违反前置条件的调用。
     */
    size_t slotIndexOfOffset(size_t slotOffset) const noexcept
    {
        const size_t quotient = rotateRight(slotOffset * slotDivisorInverse_, slotDivisorShift_);
        assert(quotient <= slotDivisorLimit_ && quotient * slotSize_ == slotOffset);
        return quotient;
    }

    /**
     * @brief 判断 slotOffset 是否为 slotSize_ 整数倍, 并在整除时给出槽索引。
     * @param[in] slotOffset 待判断的字节偏移。
     * @param[out] slotIndex 整除时写入 slotOffset / slotSize_；不整除时保持原值。
     * @retval true slotOffset 恰好落在槽边界, slotIndex 已写入。
     * @retval false slotOffset 不是 slotSize_ 的整数倍。
     * @note 本函数为 noexcept。乘以奇数部分的模逆后循环右移：整除时得到真实商
     *       (必然不大于 slotDivisorLimit_)；不整除时低位/高位混入旋转结果, 数值
     *       必然越过商上界。一次运算同时取代热路径旧有的取余判断和除法。
     */
    bool trySlotIndexOfOffset(size_t slotOffset, size_t& slotIndex) const noexcept
    {
        const size_t quotient = rotateRight(slotOffset * slotDivisorInverse_, slotDivisorShift_);
        if (quotient > slotDivisorLimit_)
            return false;
        slotIndex = quotient;
        return true;
    }

    /**
     * @brief 读取 stateFlags_ 中一个独立布尔状态位。
     * @param[in] flag 要读取的单比特掩码。
     * @retval true 对应位已设置。
     * @retval false 对应位未设置。
     * @note 本函数为 noexcept, 不读取或解释 Operation 所占的其他位。
     */
    bool hasStateFlag(PoolStateFlag flag) const noexcept
    {
        return (stateFlags_ & static_cast<PoolStateFlags>(flag)) != 0;
    }

    /**
     * @brief 设置或清除 stateFlags_ 中一个独立布尔状态位。
     * @param[in] flag 要修改的单比特掩码。
     * @param[in] enabled true 表示置位, false 表示清零。
     * @note 本函数为 noexcept, 只修改 flag 指定的位, 完整保留 Operation 编码。
     */
    void setStateFlag(PoolStateFlag flag, bool enabled) noexcept
    {
        const PoolStateFlags mask = static_cast<PoolStateFlags>(flag);
        if (enabled)
            stateFlags_ = static_cast<PoolStateFlags>(stateFlags_ | mask);
        else
            stateFlags_ = static_cast<PoolStateFlags>(stateFlags_ & static_cast<PoolStateFlags>(~mask));
    }

    /**
     * @brief 读取当前受保护操作状态。
     * @return operation_ 字节直接解码得到的 Operation 值。
     * @note 本函数为 noexcept。Operation 独占一个字节成员, 读取是单字节 load,
     *       不再需要位域掩码运算；scanOtherBlocks 提示位保存在独立的 stateFlags_ 中。
     */
    Operation operationValue() const noexcept
    {
        return static_cast<Operation>(operation_);
    }

    /**
     * @brief 写入新的受保护操作状态。
     * @param[in] operation 待保存的 Operation 枚举值。
     * @note 本函数为 noexcept。单字节 store 取代旧版"读-掩码-或-写"的位域更新,
     *       alloc/free 热路径每次状态切换只花一条指令。
     */
    void setOperation(Operation operation) noexcept
    {
        operation_ = static_cast<uint8_t>(operation);
    }

    /**
     * @brief 计算池实际保存的最终槽位跨度。
     * @param[in] requestedSize 调用方请求的原始槽尺寸, 单位: 字节；不得为 0。
     * @param[in] alignment 已规范化的槽位地址对齐值, 单位: 字节。
     * @return max(requestedSize, sizeof(ListNode)) 向上按 alignment 取整后的 32 位槽跨度。
     * @throws std::invalid_argument requestedSize 或 alignment 为 0。
     * @throws std::length_error 向上取整时超过 size_t 范围，或最终槽跨度超过 UINT32_MAX。
     */
    static uint32_t alignSlot(size_t requestedSize, size_t alignment)
    {
        if (requestedSize == 0)
            throw std::invalid_argument("槽位尺寸不能为 0");
        const size_t minimum = requestedSize < sizeof(ListNode) ? sizeof(ListNode) : requestedSize;
        const size_t aligned = checkedAlignUp(minimum, alignment, "槽位尺寸向上对齐时溢出");
        if (aligned > (std::numeric_limits<uint32_t>::max)())
            throw std::length_error("对齐后的槽位尺寸超过 32 位可记录范围");
        return static_cast<uint32_t>(aligned);
    }

    /**
     * @brief 按当前槽尺寸计算一个物理块需要的有效活动位图字节数。
     * @param[in] blockSize 物理块总尺寸, 单位: 字节。
     * @param[in] slotSize 最终对齐后的单槽跨度, 单位: 字节；必须大于 0。
     * @return ceil(floor((blockSize - sizeof(MemoryHead)) / slotSize) / 8), 单位: 字节。
     *         blockSize 不大于块头时返回 0。
     *
     * @note 先按“去掉 MemoryHead 后最多可能切出的槽数”预留一槽一 bit。该候选槽数
     *       尚未扣除位图和地址补齐, 因而不会少于最终 slotCapacity；多出的尾部 bit
     *       始终保持 0。槽尺寸改变时, 完整空块会重新计算本值和 dataBegin。
     */
    static size_t rawStateBytesForBlockSize(size_t blockSize, size_t slotSize) noexcept
    {
        if (slotSize == 0 || blockSize <= sizeof(MemoryHead))
            return 0;
        const size_t candidateSlots = (blockSize - sizeof(MemoryHead)) / slotSize;
        size_t bytes = candidateSlots / 8;
        if (candidateSlots % 8 != 0)
            ++bytes;
        return bytes;
    }

    /**
     * @brief 在不读写物理块内容的情况下计算前置位图和槽位区边界。
     * @param[in] baseAddress 物理块首地址的整数表示。
     * @param[in] blockSize 物理块总尺寸, 单位: 字节。
     * @param[in] slotSize 最终对齐后的槽跨度, 单位: 字节。
     * @param[in] alignment 槽首地址对齐值, 单位: 字节；必须是非零 2 次幂。
     * @param[out] output 成功时写入完整计算结果；失败时内容未定义。
     * @retval true 地址计算、位图范围和数据边界均可表示。
     * @retval false 参数为 0、地址加法溢出, 或位图/补齐已经超过块尾。
     *
     * @note 位图一个 bit 对应一个候选槽, 不对应 alignment 量子。改变 slotSize 后必须
     *       只在完整空块上重新调用本函数, 再更新 dataBegin/item/stateBytes。
     */
    static bool calculateBlockLayout(std::uintptr_t baseAddress, size_t blockSize,
                                     size_t slotSize, size_t alignment,
                                     BlockLayout& output) noexcept
    {
        if (slotSize == 0 || alignment == 0)
            return false;

        BlockLayout result = {};
        if (!addressAdd(baseAddress, blockSize, result.blockEnd)
            || !addressAdd(baseAddress, sizeof(MemoryHead), result.bitmapBegin)
            || result.bitmapBegin > result.blockEnd)
            return false;

        result.rawStateBytes = rawStateBytesForBlockSize(blockSize, slotSize);
        if (!addressAdd(result.bitmapBegin, result.rawStateBytes, result.dataBegin))
            return false;

        const size_t remainder = static_cast<size_t>(result.dataBegin % alignment);
        if (remainder != 0
            && !addressAdd(result.dataBegin, alignment - remainder, result.dataBegin))
            return false;
        if (result.dataBegin > result.blockEnd)
            return false;

        result.stateBytes = static_cast<size_t>(result.dataBegin - result.bitmapBegin);
        result.slotCapacity = static_cast<size_t>(result.blockEnd - result.dataBegin) / slotSize;
        output = result;
        return true;
    }

    /**
     * @brief 根据单槽尺寸计算延迟初始化首块的默认槽位数量。
     * @return max(1, min(1 MiB / slotSize_, 4096)), 单位: 槽位个数。
     * @note 本函数为 noexcept。slotSize_ 在构造阶段已保证非零。
     */
    size_t defaultInitialCount() const noexcept
    {
        size_t count = initialPayloadBudget_ / slotSize_;
        if (count == 0)
            count = 1;
        if (count > maximumInitialSlots_)
            count = maximumInitialSlots_;
        return count;
    }

    /**
     * @brief 根据旧链尾容量计算下一个扩展块的目标槽位数量。
     * @param[in] tail 当前真实链尾；必须非空且元数据有效。
     * @return min(旧容量 * 2 + 1, 16 MiB / slotSize_), 下限为 1, 单位: 槽位个数。
     * @note 乘法在执行前显式检查 size_t 上限；达到上限时直接采用预算封顶值。
     */
    size_t nextBlockCount(MemoryHead* tail) const
    {
        const size_t oldCount = slotCapacity(tail);
        size_t cappedCount = growthPayloadBudget_ / slotSize_;
        if (cappedCount == 0)
            cappedCount = 1;

        size_t candidate = cappedCount;
        if (oldCount <= ((std::numeric_limits<size_t>::max)() - 1) / 2)
        {
            candidate = oldCount * 2 + 1;
            if (candidate > cappedCount)
                candidate = cappedCount;
        }
        if (candidate == 0)
            candidate = 1;
        return candidate;
    }

    /**
     * @brief 要求当前池不处于构造、释放或批量清理重入保护期。
     * @param[in] message 条件不满足时写入 std::logic_error 的错误消息。
     * @throws std::logic_error operationValue() 不是 Operation::idle。
     */
    void requireIdle(const char* message) const
    {
        if (operationValue() != Operation::idle)
            throw std::logic_error(message);
    }

    /**
     * @brief 在移动构造 Base 前检查来源池状态并移动构造 allocator 返回值。
     * @param[in,out] other 来源池。
     * @return 从 other.allocator_ 移动构造的新 Allocator 值。
     * @throws std::logic_error other 当前不是 idle。
     * @note Allocator 移动构造在类级 static_assert 中被要求 noexcept。
     */
    static Allocator moveAllocatorChecked(CMemoryPoolBase& other)
    {
        if (other.operationValue() != Operation::idle)
        {
            assert(false && "cannot move a memory pool during a protected operation");
            throw std::logic_error("不能移动正在执行构造、释放或清理的内存池");
        }
        // 保持到外层移动构造函数体完成所有权转移后再恢复 idle；即使 C++14
        // 未消除返回值的第二次移动，allocator 回调也不能修改来源池。
        other.setOperation(Operation::managing);
        return std::move(other.allocator_);
    }

    /**
     * @brief 判断两个池是否可以安全互相释放对方拥有的物理块。
     * @param[in] other 待比较 allocator owner 的另一个池。
     * @retval true allocator 声明 is_always_equal, 或两个有状态 allocator 比较相等。
     * @retval false 有状态 allocator 比较不相等。
     * @note 有状态 Allocator 的 operator== 若抛出异常, 该异常原样传播。
     */
    bool allocatorsCompatible(const CMemoryPoolBase& other) const
    {
        using Detected = decltype(
            allocatorAlwaysEqualProbe<AllocatorTraits, Allocator>(0));
        using AlwaysEqual = std::integral_constant<bool, Detected::value>;
        return allocatorsCompatibleImpl(other, AlwaysEqual());
    }

    /**
     * @brief is_always_equal allocator 的兼容性快速分派。
     * @param[in] other 另一个池；该分支无需读取其实例状态。
     * @param[in] tag std::true_type 标签, 表示任意 allocator 实例可互相释放内存。
     * @return 始终返回 true。
     * @note 本函数为 noexcept, 时间复杂度为 O(1)。
     */
    bool allocatorsCompatibleImpl(const CMemoryPoolBase& other,
                                  std::true_type tag) const noexcept
    {
        (void)other;
        (void)tag;
        return true;
    }

    /**
     * @brief 有状态 allocator 的兼容性比较分派。
     * @param[in] other 另一个池。
     * @param[in] tag std::false_type 标签, 表示必须比较 allocator 实例 owner。
     * @return allocator_ == other.allocator_ 的结果。
     * @note Allocator::operator== 若抛出异常, 该异常原样传播。
     */
    bool allocatorsCompatibleImpl(const CMemoryPoolBase& other,
                                  std::false_type tag) const
    {
        (void)tag;
        return allocator_ == other.allocator_;
    }

    /**
     * @brief allocator 会随 swap 传播时的预检查空分派。
     * @param[in] other 另一个池；传播分支无需检查实例兼容性。
     * @param[in] tag std::true_type 标签。
     * @note 本函数为 noexcept, 不执行操作。
     */
    void prepareAllocatorSwap(const CMemoryPoolBase& other,
                              std::true_type tag) const noexcept
    {
        (void)other;
        (void)tag;
    }

    /**
     * @brief allocator 不随 swap 传播时验证两个实例能否共同管理现有块。
     * @param[in] other 另一个池。
     * @param[in] tag std::false_type 标签。
     * @throws std::invalid_argument 两个 allocator 实例不兼容。
     * @note 有状态 allocator 的相等比较若抛出异常, 该异常原样传播。
     */
    void prepareAllocatorSwap(const CMemoryPoolBase& other,
                              std::false_type tag) const
    {
        (void)tag;
        if (!allocatorsCompatible(other))
            throw std::invalid_argument("swap: allocator 不传播且彼此不兼容");
    }

    /**
     * @brief allocator 传播分支实际交换两个 allocator 实例。
     * @param[in,out] other 另一个池。
     * @param[in] tag std::true_type 标签。
     * @note 类级 static_assert 要求该 std::swap 调用 noexcept, 避免块链与 owner 失配。
     */
    void swapAllocator(CMemoryPoolBase& other, std::true_type tag)
    {
        (void)tag;
        // 与类级 noexcept(std::swap(...)) 静态约束保持完全相同的调用路径。
        std::swap(allocator_, other.allocator_);
    }

    /**
     * @brief allocator 不传播分支的交换空操作。
     * @param[in] other 另一个池；两个实例已经由 prepareAllocatorSwap() 证明兼容。
     * @param[in] tag std::false_type 标签。
     * @note 本函数为 noexcept, 不交换 allocator 实例, 只交换由兼容 owner 管理的块链。
     */
    void swapAllocator(CMemoryPoolBase& other, std::false_type tag) noexcept
    {
        (void)other;
        (void)tag;
    }

    /**
     * @brief allocator 会随移动赋值传播时的预检查空分派。
     * @param[in] other 来源池；传播分支无需比较 allocator owner。
     * @param[in] tag std::true_type 标签。
     * @note 本函数为 noexcept, 不执行操作。
     */
    void prepareAllocatorMove(const CMemoryPoolBase& other,
                              std::true_type tag) const noexcept
    {
        (void)other;
        (void)tag;
    }

    /**
     * @brief allocator 不传播时验证目标实例能否接管来源池的物理块。
     * @param[in] other 来源池。
     * @param[in] tag std::false_type 标签。
     * @throws std::invalid_argument 两个 allocator 实例不兼容。
     * @note 有状态 allocator 的相等比较若抛出异常, 该异常原样传播。
     */
    void prepareAllocatorMove(const CMemoryPoolBase& other,
                              std::false_type tag) const
    {
        (void)tag;
        if (!allocatorsCompatible(other))
            throw std::invalid_argument("move: allocator 不传播且彼此不兼容");
    }

    /**
     * @brief allocator 传播分支执行移动赋值。
     * @param[in,out] other 来源池。
     * @param[in] tag std::true_type 标签。
     * @note 类级 static_assert 要求传播型 Allocator 移动赋值 noexcept。
     */
    void moveAssignAllocator(CMemoryPoolBase& other, std::true_type tag)
    {
        (void)tag;
        allocator_ = std::move(other.allocator_);
    }

    /**
     * @brief allocator 不传播分支的移动赋值空操作。
     * @param[in] other 来源池；目标 allocator 已被证明能释放其物理块。
     * @param[in] tag std::false_type 标签。
     * @note 本函数为 noexcept, 保留目标池原有 allocator 实例。
     */
    void moveAssignAllocator(CMemoryPoolBase& other, std::false_type tag) noexcept
    {
        (void)other;
        (void)tag;
    }

    /**
     * @brief 对整数形式的地址执行不产生环绕的字节偏移加法。
     * @param[in] base 起始地址的整数表示。
     * @param[in] bytes 要前移的距离, 单位: 字节。
     * @param[out] end 成功时写入 base + bytes；失败时保持原值。
     * @retval true 地址加法没有超过 uintptr_t 上限。
     * @retval false 数学结果无法由 uintptr_t 表示。
     * @note 本函数为 noexcept, 只做整数边界计算, 不解引用地址。
     */
    static bool addressAdd(std::uintptr_t base, size_t bytes, std::uintptr_t& end) noexcept
    {
        if (bytes > (std::numeric_limits<std::uintptr_t>::max)() - base)
            return false;
        end = base + bytes;
        return true;
    }

    /**
     * @brief 判断地址是否落在一个完整物理块的半开区间内。
     * @param[in] head 物理块头, 其地址就是物理块首地址。
     * @param[in] value 待判断地址。
     * @retval true value 位于 [head, head + head->size) 内。
     * @retval false value 在块外, 或物理块尾地址计算发生整数溢出。
     * @note 本函数为 noexcept。范围包含块头、前置位图、对齐补齐和槽位数据区。
     */
    static bool containsPhysicalAddress(MemoryHead* head, BytePointer value) noexcept
    {
        const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(head);
        std::uintptr_t end = 0;
        if (!addressAdd(begin, head->size, end))
            return false;
        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(value);
        return address >= begin && address < end;
    }

    /**
     * @brief 判断地址是否落在某块 bump 高水位已经覆盖的槽位范围内。
     * @param[in] head 待检查物理块。
     * @param[in] value 待检查地址。
     * @param[out] offset 成功时写入 value - dataBegin, 单位: 字节；失败时保持原值。
     * @retval true value 位于 [dataBegin, item) 内。
     * @retval false value 位于尚未使用区、块头、位图或块外。
     * @note 本函数为 noexcept, 只检查范围, 不验证槽位边界或活动位。
     */
    bool addressInAllocatedRange(MemoryHead* head, BytePointer value,
                                 size_t& offset) const noexcept
    {
        const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(head->dataBegin);
        const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(head->item);
        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(value);
        if (address < begin || address >= end)
            return false;
        offset = static_cast<size_t>(address - begin);
        return true;
    }

    /**
     * @brief 返回一个物理块中前置活动位图的首地址。
     * @param[in] head 物理块头；位图紧接在完整 MemoryHead 对象之后。
     * @return reinterpret_cast<uint8_t*>(head) + sizeof(MemoryHead)。
     * @note 本函数为 noexcept。head->stateBytes 表示从该地址到 dataBegin 的完整对齐
     *       占位跨度, 其中原始位图字节之后可能包含对齐补齐字节。
     */
    static BytePointer stateBegin(MemoryHead* head) noexcept
    {
        return reinterpret_cast<BytePointer>(head) + sizeof(MemoryHead);
    }

    /** @brief 判断字节偏移是否仍落在本块数据区的可表示范围内。 */
    bool slotOffsetWithinDataRegion(MemoryHead* head, size_t slotOffset) const noexcept
    {
        const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(head->dataBegin);
        const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(head->dataEnd);
        return end >= begin && slotOffset < static_cast<size_t>(end - begin);
    }

    /** @brief 读取已经验证过范围的零基槽索引对应活动位。 */
    bool isStateIndexInUse(MemoryHead* head, size_t slotIndex) const noexcept
    {
        const size_t byteIndex = slotIndex >> 3;
        if (byteIndex >= head->stateBytes)
            return false;
        const uint8_t mask = static_cast<uint8_t>(1U << (slotIndex & 7));
        return (stateBegin(head)[byteIndex] & mask) != 0;
    }

    /** @brief 修改已经验证过范围的零基槽索引对应活动位。 */
    void setStateIndexInUse(MemoryHead* head, size_t slotIndex, bool inUse) noexcept
    {
        const size_t byteIndex = slotIndex >> 3;
        assert(byteIndex < head->stateBytes);
        if (byteIndex >= head->stateBytes)
            return;

        const uint8_t mask = static_cast<uint8_t>(1U << (slotIndex & 7));
        if (inUse)
            stateBegin(head)[byteIndex] |= mask;
        else
            stateBegin(head)[byteIndex] &= static_cast<uint8_t>(~mask);
    }

    /**
     * @brief 读取一个槽位对应的持久活动位。
     * @param[in] head 槽位所属物理块。
     * @param[in] slotOffset 槽位相对 dataBegin 的偏移, 单位: 字节。
     * @retval true 对应位图 bit 为 1。
     * @retval false 对应 bit 为 0, 或 slotOffset 无法安全映射到有效位图。
     * @note 本函数为 noexcept，会验证 slotOffset 位于完整槽首；异常偏移直接返回 false。
     */
    bool isSlotInUse(MemoryHead* head, size_t slotOffset) const noexcept
    {
        // 受保护接口也拒绝非槽首和尾部不足一槽的偏移，避免派生类误把位图补齐
        // 或数据区尾部余量映射成一个不存在的槽位状态。
        // 槽边界判断与索引换算由乘法逆一次完成, 不再执行取余和除法。
        size_t slotIndex = 0;
        if (!trySlotIndexOfOffset(slotOffset, slotIndex)
            || !slotOffsetWithinDataRegion(head, slotOffset))
            return false;
        const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(head->dataBegin);
        const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(head->dataEnd);
        const size_t dataBytes = static_cast<size_t>(end - begin);
        if (dataBytes - slotOffset < slotSize_)
            return false;
        return isStateIndexInUse(head, slotIndex);
    }

    /**
     * @brief 设置一个槽位对应的持久活动位。
     * @param[in,out] head 槽位所属物理块。
     * @param[in] slotOffset 槽位相对 dataBegin 的偏移, 单位: 字节。
     * @param[in] inUse true 表示写入活动状态, false 表示清除活动状态。
     * @note 本函数为 noexcept。Debug 断言 slotOffset 对应数据区中的完整槽；Release
     *       对非槽首和尾部余量直接返回，保证不会把位图补齐区当作活动状态写入。
     */
    void setSlotInUse(MemoryHead* head, size_t slotOffset, bool inUse) noexcept
    {
        // 与读取路径使用同一完整槽边界；Debug 提示内部偏移错误，Release 安全返回。
        // 槽边界与索引由乘法逆一次得到, 免去取余判断。
        size_t slotIndex = 0;
        const bool startsCompleteSlot = trySlotIndexOfOffset(slotOffset, slotIndex)
            && slotOffsetWithinDataRegion(head, slotOffset)
            && static_cast<size_t>(head->dataEnd - head->dataBegin) - slotOffset >= slotSize_;
        assert(startsCompleteSlot);
        if (!startsCompleteSlot)
            return;
        setStateIndexInUse(head, slotIndex, inUse);
    }

    /**
     * @brief 检查一个物理块的整份活动位图是否全为零。
     * @param[in] head 待检查物理块。
     * @retval true 原始有效位图字节全部为 0。
     * @retval false 至少存在一个置位, 表示仍有已提交活动槽。
     * @note 本函数为 noexcept, 时间复杂度为 O(位图字节数)。
     */
    bool bitmapAllClear(MemoryHead* head) const noexcept
    {
        const BytePointer bitmap = stateBegin(head);
        const size_t rawBytes = rawStateBytesForBlockSize(head->size, slotSize_);
        for (size_t index = 0; index < rawBytes; ++index)
        {
            if (bitmap[index] != 0)
                return false;
        }
        return true;
    }

    /**
     * @brief 把一个物理块的整份持久活动位图清零。
     * @param[in,out] head 待重置物理块。
     * @note 本函数为 noexcept, 只清除真正保存活动 bit 的原始位图字节。stateBytes 中
     *       仅用于把 dataBegin 推到正确地址的补齐区从不保存状态, 避免高对齐小块在
     *       每次整体回收时无意义地清写最多一个对齐跨度。
     */
    void clearSlotStates(MemoryHead* head) const noexcept
    {
        const size_t rawBytes = rawStateBytesForBlockSize(head->size, slotSize_);
        std::memset(stateBegin(head), 0, rawBytes);
    }

    /**
     * @brief 计算一个物理块在当前 slotSize_ 下可容纳的完整槽位数量。
     * @param[in] head 待计算物理块。
     * @return floor((dataEnd - dataBegin) / slotSize_), 单位: 槽位个数；边界逆序时返回 0。
     * @note 本函数为 noexcept。resize_slot 后同一物理块的结果可能改变。
     */
    size_t slotCapacity(MemoryHead* head) const noexcept
    {
        const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(head->dataBegin);
        const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(head->dataEnd);
        return end >= begin ? static_cast<size_t>(end - begin) / slotSize_ : 0;
    }

    /**
     * @brief 计算 bump 高水位已经覆盖的槽位数量。
     * @param[in] head 待计算物理块。
     * @return floor((item - dataBegin) / slotSize_), 单位: 槽位个数；边界逆序时返回 0。
     * @note 本函数为 noexcept；合法元数据下 item 与 dataBegin 的差必为 slotSize_ 整数倍。
     */
    size_t highWaterSlots(MemoryHead* head) const noexcept
    {
        const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(head->dataBegin);
        const std::uintptr_t item = reinterpret_cast<std::uintptr_t>(head->item);
        return item >= begin ? static_cast<size_t>(item - begin) / slotSize_ : 0;
    }

    /**
     * @brief 在指定物理块中查询地址是否对应活动槽位首地址。
     * @param[in] head 待查询物理块。
     * @param[in] value 待查询地址。
     * @retval true 地址处于已覆盖范围、恰好位于槽边界且活动位为 1。
     * @retval false 任一条件不满足。
     * @note 本函数为 noexcept, 时间复杂度为 O(1)。
     */
    bool queryBlock(MemoryHead* head, BytePointer value) const noexcept
    {
        // 范围命中后, 槽边界判定和索引换算由乘法逆一次完成, 全程无硬件除法。
        size_t offset = 0;
        if (!addressInAllocatedRange(head, value, offset))
            return false;
        size_t slotIndex = 0;
        if (!trySlotIndexOfOffset(offset, slotIndex))
            return false;
        return isStateIndexInUse(head, slotIndex);
    }

    /**
     * @brief 判断一个物理块是否恢复到可安全重新解释的完整空状态。
     * @param[in] head 待检查物理块。
     * @retval true item 回到 dataBegin、free-list 为空、freeCount 为 0 且位图全零。
     * @retval false 任一状态仍表示已覆盖或活动槽位。
     * @note 本函数为 noexcept, 会扫描整份位图。
     */
    bool isBlockCompletelyEmpty(MemoryHead* head) const noexcept
    {
        return head->item == head->dataBegin
            && head->freeList == nullptr
            && head->freeCount == 0
            && bitmapAllClear(head);
    }

    /**
     * @brief 判断块链中的每个物理块是否均为完整空状态。
     * @retval true 空链或全部物理块完整为空。
     * @retval false 至少一个物理块不满足完整空条件。
     * @note 本函数为 noexcept, 复杂度为 O(块数 + 位图总字节数)。
     */
    bool allBlocksCompletelyEmpty() const noexcept
    {
        for (MemoryHead* head = memory_; head; head = head->next)
        {
            if (!isBlockCompletelyEmpty(head))
                return false;
        }
        return true;
    }

    /**
     * @brief 把已经完成对象析构的物理块重置为初始 bump 分配状态。
     * @param[in,out] head 待重置物理块。
     * @post item == dataBegin, freeList == nullptr, freeCount == 0, 活动位图全零。
     * @note 本函数为 noexcept, 不释放物理块, 也不调用用户对象析构函数。
     */
    void resetBlock(MemoryHead* head) noexcept
    {
        head->item = head->dataBegin;
        head->freeList = nullptr;
        head->freeCount = 0;
        clearSlotStates(head);
    }

    /**
     * @brief 预留一个原始槽位, 但暂不写入活动位。
     *
     * 分配顺序为: current_ 的连续未使用区、current_ 的 free-list、按热点后继顺序扫描
     * 其他旧块、申请并 O(1) 追加新块。scanOtherBlocks 位记录"其他旧块是否可能存在
     * 空位", 避免旧块已被证明全满后每次分配都重复 O(块数) 扫描。
     *
     * @return 包含预留地址、来源类型以及构造失败回滚所需快照的 RawReservation。
     * @throws std::logic_error free-list 非空但 freeCount 为 0, 表示元数据损坏。
     * @throws std::length_error 新物理块的尺寸计算溢出或超过 allocator 上限。
     * @throws std::runtime_error 无法取得有效的系统页大小。
     * @throws std::bad_alloc 新物理块分配失败或页对齐契约不成立。
     *
     * @pre 对象池调用时 operation 为 constructing；字节池快路径调用时 operation 为
     *      idle, 仅扩块段由内部 ConditionalManagingScope 临时进入 managing。
     * @note 返回时对应活动位仍为 0, 调用方必须提交或回滚, 不能把该地址直接泄露为
     *       已构造对象。自定义 Allocator::allocate 的其他异常原样传播。热点命中为 O(1),
     *       需要扫描旧块时最坏为 O(物理块数量)。
     */
    RawReservation reserveSlot()
    {
#if defined(MEMORYPOOL_TEST_DIAGNOSTICS)
        diagnosticLastScanCount_ = 0;
#endif
        // 快照只保存回滚真正需要的链指针和扫描提示；Operation 由事务调用方统一管理。
        RawReservation reservation = {};
        reservation.previousCurrent = current_;
        reservation.previousTail = tail_;
        reservation.previousScanFlags = static_cast<PoolStateFlags>(
            stateFlags_ & static_cast<PoolStateFlags>(PoolStateFlag::scanOtherBlocks));

        if (!memory_)
        {
            // 延迟初始化路径: 首个槽位来自新建首块, 失败回滚时整块直接释放。
            MemoryHead* head;
            {
                // 建块会执行 allocator 用户代码；字节池快路径此刻为 idle,
                // 这里临时进入 managing 拒绝回调重入, 对象池事务则保持 constructing。
                ConditionalManagingScope guard(*this);
                head = mallocHead(defaultInitialCount());
            }
            memory_ = head;
            tail_ = head;
            current_ = head;
            setStateFlag(PoolStateFlag::scanOtherBlocks, false);
            reservation.head = head;
            reservation.address = head->item;
            reservation.origin = ReservationOrigin::newBlock;
            head->item += slotSize_;
            return reservation;
        }

        // 热路径始终只访问 current_, 通常无需任何块链遍历。
        if (tryReserveFromBlock(current_, reservation))
            return reservation;

        // 只有发生过 free/clear/merge/split 等可能制造空位的操作后, 才重新扫描旧块。
        // 从 current_ 的后继开始并在链首回绕, 可让 clear 后的多块顺序复用总计接近 O(B)。
        if (hasStateFlag(PoolStateFlag::scanOtherBlocks))
        {
            for (MemoryHead* head = current_->next; head; head = head->next)
            {
#if defined(MEMORYPOOL_TEST_DIAGNOSTICS)
                ++diagnosticLastScanCount_;
#endif
                if (tryReserveFromBlock(head, reservation))
                    return reservation;
            }
            for (MemoryHead* head = memory_; head && head != current_; head = head->next)
            {
#if defined(MEMORYPOOL_TEST_DIAGNOSTICS)
                ++diagnosticLastScanCount_;
#endif
                if (tryReserveFromBlock(head, reservation))
                    return reservation;
            }
            // 本次已经证明除 current_ 外的旧块均不可分配；在下一次制造空位前无需重复扫描。
            setStateFlag(PoolStateFlag::scanOtherBlocks, false);
        }

        // current_ 与允许扫描的全部旧块均无空间, 按增长策略建立并追加唯一的新块。
        MemoryHead* head;
        {
            // 与延迟初始化路径相同：只在 allocator 用户代码执行期间临时保护。
            ConditionalManagingScope guard(*this);
            head = mallocHead(nextBlockCount(tail_));
        }
        tail_->next = head;
        tail_ = head;
        current_ = head;
        setStateFlag(PoolStateFlag::scanOtherBlocks, false);
        reservation.head = head;
        reservation.address = head->item;
        reservation.origin = ReservationOrigin::newBlock;
        head->item += slotSize_;
        return reservation;
    }

    /**
     * @brief 尝试从一个指定物理块预留槽位。
     * @param[in,out] head 候选物理块；成功时会推进 item 或摘除一个 free-list 节点。
     * @param[in,out] reservation 事务记录；成功时写入 head、address 和 origin, 其他快照保留。
     * @retval true 已从连续未使用区或 free-list 取得一个槽位, 并把 current_ 更新为 head。
     * @retval false 该块既没有完整连续槽, 也没有 free-list 节点。
     * @throws std::logic_error freeList 非空但 freeCount 为 0。
     * @note 优先使用 bump 区以保持连续访问和缓存局部性, 只有 bump 耗尽后才复用链表节点。
     */
    bool tryReserveFromBlock(MemoryHead* head, RawReservation& reservation)
    {
        const std::uintptr_t item = reinterpret_cast<std::uintptr_t>(head->item);
        const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(head->dataEnd);
        if (end >= item && static_cast<size_t>(end - item) >= slotSize_)
        {
            // 连续区命中只前移一次 bump 高水位, 回滚时可用 reservation.address 精确撤销。
            reservation.head = head;
            reservation.address = head->item;
            reservation.origin = ReservationOrigin::bump;
            head->item += slotSize_;
            // 命中的通常就是 current_ 自身；先比较再写, 让纯 bump 分配路径
            // 不弄脏池对象所在缓存行。
            if (current_ != head)
                current_ = head;
            return true;
        }

        if (head->freeList)
        {
            // free-list 计数是关键守恒量；先验证再摘节点, 避免无符号下溢掩盖损坏。
            if (head->freeCount == 0)
                throw std::logic_error("free-list 与 freeCount 不一致");
            ListNode* node = head->freeList;
            head->freeList = node->next;
            --head->freeCount;
            node->~ListNode();

            reservation.head = head;
            reservation.address = reinterpret_cast<BytePointer>(node);
            reservation.origin = ReservationOrigin::freeList;
            if (current_ != head)
                current_ = head;
            return true;
        }
        return false;
    }

    /**
     * @brief 尝试把地址作为指定物理块中的一个活动槽位释放。
     * @tparam Hook 编译期确定的释放 hook 类型；见 freeWithHook() 的约束说明。
     * @param[in,out] head 候选物理块。
     * @param[in] value 待释放地址。
     * @param[in] hook 完成对象析构(或空操作)的可调用对象。
     * @retval true value 是该块中的活动槽首, 且 hook 与槽位回收均已完成。
     * @retval false 地址不在已覆盖区、不是槽边界, 或活动位已经为 0。
     * @note 本函数为 noexcept。未定义 NDEBUG 时, [dataBegin,item) 内可确认的未对齐
     *       地址和非活动槽会触发 assert。hook 会执行用户代码时, 调用前先清活动位并
     *       设置 releasing 拒绝同池析构重入；hook 为空操作时该状态切换被编译期裁掉。
     *       全路径无硬件除法, 整块折叠也不再清位图, 成功释放始终为 O(1)。
     */
    template<class Hook>
    bool tryFreeBlock(MemoryHead* head, BytePointer value, const Hook& hook) noexcept
    {
        // 第一阶段只做范围、槽边界和持久活动位验证, 失败时绝不修改块元数据。
        size_t offset = 0;
        if (!addressInAllocatedRange(head, value, offset))
            return false;

        // 乘法逆一步完成"是否槽边界"判定和索引换算, 取代旧版的除法加乘回校验。
        size_t slotIndex = 0;
        if (!trySlotIndexOfOffset(offset, slotIndex))
        {
            assert(false && "free address is not aligned to slot boundary");
            return false;
        }
        if (!isStateIndexInUse(head, slotIndex))
        {
            assert(false && "free address does not refer to an in-use slot");
            return false;
        }

        // hook 会进入用户析构代码时, 先阻止同池重入再清活动位；
        // 空 hook(字节池/平凡析构)在编译期裁掉这两次状态切换。
        if (Hook::invokes_user_code::value)
            setOperation(Operation::releasing);
        setStateIndexInUse(head, slotIndex, false);
        hook(value);

        if (head->item == value + slotSize_)
        {
            // 最新的 bump 槽可直接回退高水位, 无需在用户槽内构造链表节点。
            head->item = value;

            // 回退后高水位槽数恰好等于 slotIndex, 无需任何除法即可判断整块折叠。
            // freeCount 为 0 是 LIFO 的常见情形：块内不存在空洞, 直接零额外工作返回。
            if (head->freeCount != 0 && head->freeCount == slotIndex)
                collapseFullyFreedBlock(head);
        }
        else
        {
            // 非尾部洞无法移动后续槽, 只能复用槽自身存储链接到 free-list 表头。
            ListNode* node = ::new (static_cast<void*>(value)) ListNode;
            node->next = head->freeList;
            head->freeList = node;
            ++head->freeCount;

            // 高水位槽数由乘法逆精确除法求得(item 与 dataBegin 的差必为槽尺寸整数倍)。
            // 当高水位内所有槽都位于 free-list 时, 整块折叠回最干净的 bump 初态。
            if (head->freeCount == slotIndexOfOffset(
                    static_cast<size_t>(head->item - head->dataBegin)))
                collapseFullyFreedBlock(head);
        }

        if (Hook::invokes_user_code::value)
            setOperation(Operation::idle);
        return true;
    }

    /**
     * @brief 把"高水位内全部槽都已逐个释放"的物理块折叠回 bump 初态。
     * @param[in,out] head 待折叠物理块；freeCount 必须等于当前高水位槽数。
     * @post item == dataBegin, freeList == nullptr, freeCount == 0。
     * @note 本函数为 noexcept 且 O(1)。与 resetBlock() 不同, 这里不清位图：每次
     *       tryFreeBlock 成功时都已把对应活动位清零, 高水位之上的位从未置位,
     *       因此折叠时整份位图必然已经全 0, 旧版的整块 memset 是冗余工作。
     */
    void collapseFullyFreedBlock(MemoryHead* head) noexcept
    {
        assert(bitmapAllClear(head));
        head->item = head->dataBegin;
        head->freeList = nullptr;
        head->freeCount = 0;
    }

    /**
     * @brief 分配并初始化一个"块首和总尺寸均按系统页对齐"的物理块。
     *
     * 内存布局为 [MemoryHead][持久位图及其对齐补齐][槽位数据区]。位图紧接块头,
     * stateBytes 记录从位图首地址到 dataBegin 的完整占位跨度；例如原始位图需要
     * 31 字节时, 占位会继续补齐, 不会让后续数据从未对齐地址开始。位图和补齐都
     * 位于同一次 allocator 分配中, 物理块总尺寸始终按真实系统页大小向上取整。
     *
     * @param[in] count 该块至少需要容纳的完整槽位数量, 单位: 个；必须大于 0。
     * @return 已 placement-new 的 MemoryHead 指针；该指针同时是页对齐物理块首地址。
     * @throws std::invalid_argument count 为 0。
     * @throws std::runtime_error 无法取得非零系统页大小。
     * @throws std::length_error 槽字节、块头、填充、位图、页取整或地址计算溢出,
     *                           物理块超过 allocator::max_size(), 或最终布局无法容纳 count。
     * @throws std::bad_alloc allocator 分配失败、返回 nullptr, 或返回地址不满足系统页对齐。
     *
     * @post 返回块 next 为空, item == dataBegin, free-list 为空, freeCount 为 0,
     *       整份持久活动位图已清零。
     * @note 自定义 AllocatorTraits::allocate 若抛出其他异常, 该异常原样传播。
     */
    MemoryHead* mallocHead(size_t count)
    {
        if (count == 0)
            throw std::invalid_argument("物理块至少需要容纳一个槽位");

        // 物理块页对齐不能依赖硬编码 4096, 必须使用当前系统真实页大小。
        const size_t pageSize = memory_pool_system_page_size();
        if (pageSize == 0)
            throw std::runtime_error("无法取得有效的系统内存页大小");
        const size_t slotAlignment = slotAlignmentValue();

        // 先计算满足 count 的固定部分: 块头、位图后最坏对齐补齐和完整槽位 payload。
        // 这里的 A-1 是尚未取得真实块首地址时必须预留的最坏补齐；实际 padding 可能更小。
        const size_t dataBytes = checkedMultiply(count, slotSize_, "槽位数量乘以槽位尺寸时溢出");
        size_t fixedBytes = checkedAdd(sizeof(MemoryHead), slotAlignment - 1,
                                       "块头与槽位对齐填充相加时溢出");
        fixedBytes = checkedAdd(fixedBytes, dataBytes, "物理块最小数据尺寸溢出");

        // 一槽一 bit 的实际位图不超过 ceil(T/(8*S)) 字节。为在取得真实块地址前仍保证
        // 至少容纳 count 个槽，令 G=8*S，并求解保守条件 T >= fixedBytes + ceil(T/G)：
        //   T >= fixedBytes + ceil(fixedBytes/(G-1))
        // 先求不含页取整的闭式上界，再一次按页取整；分配后会按真实候选槽数重算 L。
        size_t bitmapAllowance = 1;
        if (slotSize_ <= (std::numeric_limits<size_t>::max)() / 8)
        {
            const size_t addressBytesPerBitmapByte = static_cast<size_t>(slotSize_) * 8;
            const size_t denominator = addressBytesPerBitmapByte - 1;
            bitmapAllowance = fixedBytes / denominator;
            if (fixedBytes % denominator != 0)
                ++bitmapAllowance;
        }
        // 当 8*S 超过 size_t 范围时，任意可表示物理块的一槽一 bit 位图都不超过 1 字节。
        const size_t minimumBytes = checkedAdd(fixedBytes, bitmapAllowance,
                                               "物理块加入持久位图时溢出");
        const size_t totalSize = checkedAlignUp(minimumBytes, pageSize,
                                                "物理块加入位图后按页取整时溢出");
        const size_t rawStateBytes = rawStateBytesForBlockSize(totalSize, slotSize_);
        const size_t requiredBytes = checkedAdd(fixedBytes, rawStateBytes,
                                                "物理块复核持久位图尺寸时溢出");
        if (requiredBytes > totalSize)
            throw std::length_error("物理块闭式位图尺寸复核失败");

        // allocator 的 count 单位是单字节元素, 因此 totalSize 可直接作为分配数量。
        if (totalSize > AllocatorTraits::max_size(allocator_))
            throw std::length_error("物理块字节数超过 allocator::max_size");

        const AllocatorSize allocatorCount = static_cast<AllocatorSize>(totalSize);
        AllocatorPointer allocated = AllocatorTraits::allocate(allocator_, allocatorCount);
        BytePointer blockBase = reinterpret_cast<BytePointer>(allocated);
        if (!blockBase)
            throw std::bad_alloc();

        // 对 allocator 契约做运行时防御: 块首和块长都必须满足页边界要求。
        const std::uintptr_t baseAddress = reinterpret_cast<std::uintptr_t>(blockBase);
        if (baseAddress % pageSize != 0
            || baseAddress % alignof(MemoryHead) != 0
            || totalSize % pageSize != 0)
        {
            AllocatorTraits::deallocate(allocator_, allocated, allocatorCount);
            throw std::bad_alloc();
        }

        // 总尺寸已确定后，按候选槽数量一次得到位图字节、dataBegin 和最终容量。
        BlockLayout layout = {};
        if (!calculateBlockLayout(baseAddress, totalSize, slotSize_, slotAlignment, layout))
        {
            AllocatorTraits::deallocate(allocator_, allocated, allocatorCount);
            throw std::length_error("物理块边界计算失败");
        }

        if (layout.rawStateBytes != rawStateBytes
            || rawStateBytes > layout.stateBytes
            || layout.stateBytes % alignof(ListNode) != 0
            || layout.slotCapacity < count
            || dataBytes > layout.blockEnd - layout.dataBegin)
        {
            AllocatorTraits::deallocate(allocator_, allocated, allocatorCount);
            throw std::length_error("前置位图对齐占位后无法容纳请求的全部槽位");
        }

        // 所有可能失败的布局检查均已完成, 最后才在页首构造块头并初始化持久状态。
        MemoryHead* head = ::new (static_cast<void*>(blockBase)) MemoryHead;
        head->next = nullptr;
        head->size = totalSize;
        head->dataBegin = reinterpret_cast<BytePointer>(layout.dataBegin);
        head->item = head->dataBegin;
        head->dataEnd = reinterpret_cast<BytePointer>(layout.blockEnd);
        head->stateBytes = layout.stateBytes;
        head->freeList = nullptr;
        head->freeCount = 0;
        clearSlotStates(head);
        return head;
    }

    /**
     * @brief 析构块头并把整个物理块归还给创建它的兼容 allocator。
     * @param[in] head 待归还物理块首地址；必须非空, 且块中用户对象已经全部析构。
     * @note 本函数为 noexcept。先保存 head->size, 随后析构 MemoryHead, 最后以完全相同的
     *       元素数量调用 deallocate；对默认字节 allocator, 该数量也就是字节数。自定义
     *       allocator 的 deallocate 不得让异常逃逸，否则按 noexcept 规则终止进程。
     */
    void releaseBlockMemory(MemoryHead* head) noexcept
    {
        const size_t bytes = head->size;
        AllocatorPointer pointer = reinterpret_cast<AllocatorPointer>(head);
        head->~MemoryHead();
        AllocatorTraits::deallocate(allocator_, pointer, static_cast<AllocatorSize>(bytes));
    }

    /**
     * @brief 把一条已经脱离其他池的物理块链追加到当前链尾。
     * @param[in,out] head 待接管链首；nullptr 表示无操作。调用后当前池取得整条链所有权。
     * @param[in] appendedTail 输入链的真实尾节点；head 非空时必须非空且可由 head 到达。
     * @post tail_ 指向追加后真实链尾；原池为空时 current_ 指向新链首；扫描提示置位。
     * @note 本函数为 noexcept。调用方在重布局链时已经得到真实尾节点，因此追加本身 O(1)；
     *       启用 MEMORYPOOL_SORT_AFTER_MERGE_SPLIT 时仍会进入可选排序冷路径。
     */
    void appendBlocks(MemoryHead* head, MemoryHead* appendedTail) noexcept
    {
        if (!head)
        {
            assert(appendedTail == nullptr);
            return;
        }
        assert(appendedTail != nullptr && appendedTail->next == nullptr);

        // 空目标接管链首；非空目标利用 tail_ 直接连接, 不再遍历当前已有块链。
        const bool wasEmpty = memory_ == nullptr;
        // 新块均可能包含当前槽尺寸下的可用空间, 允许后续分配扫描其他块。
        if (wasEmpty)
        {
            memory_ = head;
        }
        else
        {
            tail_->next = head;
        }
        tail_ = appendedTail;

        if (wasEmpty)
            current_ = head;
        setStateFlag(PoolStateFlag::scanOtherBlocks, true);

#if MEMORYPOOL_SORT_AFTER_MERGE_SPLIT
        sortBySize();
#endif
    }

    /**
     * @brief 按当前槽尺寸重新建立一个完整空物理块的位图和数据区边界。
     *
     * @param[in,out] head 待重建块。调用前该块必须已经由旧池证明为完整空块；本函数
     *                     不按当前 slotSize_ 再解释旧位图，因为旧位图可能使用不同槽尺寸。
     * @retval true 新布局至少能容纳一个完整槽，块头、位图和数据区边界已经更新。
     * @retval false 新布局没有合法的对齐数据区，或无法容纳一个完整槽；块内容保持不变。
     *
     * @post 成功时 item == dataBegin，freeList == nullptr，freeCount == 0，新尺寸对应的
     *       有效位图全部清零。物理块首地址、总尺寸和 next 链接均不改变。
     * @note 本函数为 noexcept，只能用于没有活动对象、没有 free-list 节点的完整空块。
     */
    bool rebuildEmptyBlockLayout(MemoryHead* head) noexcept
    {
        BlockLayout layout = {};
        if (!calculateBlockLayout(reinterpret_cast<std::uintptr_t>(head), head->size,
                                  slotSize_, slotAlignmentValue(), layout)
            || layout.slotCapacity == 0)
            return false;

        // 先提交新边界，再按新槽数量清位图。缩小槽尺寸时位图可能进入旧数据区，
        // 扩大槽尺寸时位图可能缩短；块完整为空保证两种覆盖都不破坏活动对象。
        head->dataBegin = reinterpret_cast<BytePointer>(layout.dataBegin);
        head->item = head->dataBegin;
        head->dataEnd = reinterpret_cast<BytePointer>(layout.blockEnd);
        head->stateBytes = layout.stateBytes;
        head->freeList = nullptr;
        head->freeCount = 0;
        clearSlotStates(head);
        return true;
    }

    /**
     * @brief 按当前槽尺寸重建一条完整空块链，并释放无法容纳新槽的块。
     *
     * @param[in,out] head 输入块链首。链中每个块都必须已按原所属池验证为完整空块。
     * @param[out] rebuiltTail 返回保留链的真实尾节点；没有保留块时返回 nullptr。
     * @return 保留并完成重布局后的块链首；所有块都不可用时返回 nullptr。
     * @note 本函数为 noexcept，保持保留块的相对顺序。被删除块使用当前兼容 allocator
     *       归还；调用方负责重建所属池的 memory_、tail_、current_ 和扫描提示。
     */
    MemoryHead* rebuildEmptyChainForCurrentSlot(MemoryHead* head,
                                                 MemoryHead*& rebuiltTail) noexcept
    {
        MemoryHead* rebuiltHead = nullptr;
        rebuiltTail = nullptr;

        while (head)
        {
            MemoryHead* next = head->next;
            head->next = nullptr;

            if (rebuildEmptyBlockLayout(head))
            {
                if (rebuiltTail)
                    rebuiltTail->next = head;
                else
                    rebuiltHead = head;
                rebuiltTail = head;
            }
            else
            {
                // 块头仍保留旧的有效 size，可直接按原分配字节数归还。
                releaseBlockMemory(head);
            }
            head = next;
        }
        return rebuiltHead;
    }

#if MEMORYPOOL_SORT_AFTER_MERGE_SPLIT
    /**
     * @brief 使用稳定所有权的链表插入排序按物理块总尺寸升序重排块链。
     * @note 本函数为 noexcept, 仅在 MEMORYPOOL_SORT_AFTER_MERGE_SPLIT 非零时编译。
     *       不移动物理内存, 只修改 next；最坏时间复杂度 O(物理块数量²)。完成后重建
     *       memory_、tail_ 和扫描提示, 原 current_ 节点地址仍保持有效。
     */
    void sortBySize() noexcept
    {
        if (!memory_ || !memory_->next)
            return;

        // 逐个从旧链摘取节点, 并插入到新链中第一个尺寸不小于它的位置之前。
        MemoryHead* unsorted = memory_;
        MemoryHead* sorted = nullptr;
        while (unsorted)
        {
            MemoryHead* next = unsorted->next;
            if (!sorted || unsorted->size < sorted->size)
            {
                unsorted->next = sorted;
                sorted = unsorted;
            }
            else
            {
                MemoryHead* position = sorted;
                while (position->next && position->next->size <= unsorted->size)
                    position = position->next;
                unsorted->next = position->next;
                position->next = unsorted;
            }
            unsorted = next;
        }
        // 排序完成后线性恢复真实链尾；current_ 仅在异常空状态下回退到新链首。
        memory_ = sorted;
        tail_ = memory_;
        while (tail_ && tail_->next)
            tail_ = tail_->next;
        if (!current_)
            current_ = memory_;
        setStateFlag(PoolStateFlag::scanOtherBlocks, memory_ && memory_->next);
    }
#endif

    /**
     * @brief 验证单个物理块的页布局、槽位边界、活动位图和 free-list 数量守恒。
     * @param[in] head 待验证块头；必须来自当前池可达块链。
     * @retval true 物理块全部局部不变量成立。
     * @retval false 页对齐、边界、尺寸、位图、活动位或 free-list 任一不变量失败。
     * @note 本函数为 noexcept, 不修改块内容；复杂度为 O(位图字节数 + free-list 节点数)。
     */
    bool validateBlock(MemoryHead* head) const noexcept
    {
        // 第一阶段验证物理分配契约: 页首、块头自然对齐、总页数和块尾地址必须有效。
        const size_t pageSize = memory_pool_system_page_size();
        const size_t slotAlignment = slotAlignmentValue();
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(head);
        std::uintptr_t blockEnd = 0;
        if (pageSize == 0
            || base % pageSize != 0
            || base % alignof(MemoryHead) != 0
            || head->size % pageSize != 0
            || !addressAdd(base, head->size, blockEnd))
            return false;

        // 第二阶段按当前 slotSize_ 重新计算 [MemoryHead][一槽一 bit 位图][补齐][data]。
        // resize/merge/split 如果遗漏了空块重布局，这里的期望边界会立即与块头不一致。
        BlockLayout expected = {};
        if (!calculateBlockLayout(base, head->size, slotSize_, slotAlignment, expected)
            || expected.slotCapacity == 0)
            return false;

        // 第三阶段验证位图占位、槽位区和 bump 高水位的单调半开区间关系。
        const std::uintptr_t dataBegin = reinterpret_cast<std::uintptr_t>(head->dataBegin);
        const std::uintptr_t item = reinterpret_cast<std::uintptr_t>(head->item);
        const std::uintptr_t dataEnd = reinterpret_cast<std::uintptr_t>(head->dataEnd);
        if (dataBegin != expected.dataBegin
            || dataBegin % slotAlignment != 0
            || dataBegin > item || item > dataEnd || dataEnd != expected.blockEnd
            || expected.blockEnd != blockEnd
            || head->stateBytes != expected.stateBytes
            || head->stateBytes < expected.rawStateBytes
            || head->stateBytes - expected.rawStateBytes >= slotAlignment
            || head->stateBytes % alignof(ListNode) != 0
            || static_cast<size_t>(item - dataBegin) % slotSize_ != 0)
            return false;

        const size_t highWater = static_cast<size_t>(item - dataBegin) / slotSize_;
        if (head->freeCount > highWater)
            return false;

        // 只遍历真正保存活动 bit 的原始位图；其后的地址对齐补齐不承载状态。
        // 每个置位必须映射到高水位内的真实槽位首地址。
        size_t activeCount = 0;
        const BytePointer bitmap = stateBegin(head);
        for (size_t byteIndex = 0; byteIndex < expected.rawStateBytes; ++byteIndex)
        {
            uint8_t bits = bitmap[byteIndex];
            for (unsigned bit = 0; bits != 0 && bit < 8; ++bit)
            {
                const uint8_t mask = static_cast<uint8_t>(1U << bit);
                if ((bits & mask) == 0)
                    continue;
                bits &= static_cast<uint8_t>(~mask);

                if (byteIndex > (std::numeric_limits<size_t>::max)() / 8)
                    return false;
                const size_t slotIndex = byteIndex * 8 + bit;
                if (slotIndex >= highWater)
                    return false;
                ++activeCount;
            }
        }

        // 遍历 free-list: 节点必须位于高水位内的槽首, 且不能同时仍标记为活动。
        size_t listCount = 0;
        for (ListNode* node = head->freeList; node; node = node->next)
        {
            if (++listCount > highWater)
                return false;
            const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(node);
            if (address < dataBegin || address >= item)
                return false;
            const size_t offset = static_cast<size_t>(address - dataBegin);
            if (offset % slotSize_ != 0
                || isStateIndexInUse(head, offset / slotSize_))
                return false;
        }

        // 高水位内每个槽必须恰好属于"活动位"或"free-list"二者之一。
        return listCount == head->freeCount
            && activeCount + head->freeCount == highWater;
    }
};

NAMESPACE_MEMORYPOOL_END
