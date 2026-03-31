#pragma once
#include "CMemoryPoolBase.h"
#include <cassert>
#include <type_traits>
#include <new>
#include <cstring>
NAMESPACE_MEMORYPOOL_BEGIN



// 定长内存池, 每次分配都是固定大小的内存
template<class _Ty = uint8_t, class _Alloc = CMemoryPoolAllocator<uint8_t>>
class CMemoryObjectPool
    : public CMemoryPoolBase<_Alloc>
{
public:
    using value_type    = _Ty;
    using pointer       = _Ty*;
    using const_pointer = const _Ty*;

private:
    using typename CMemoryPoolBase<_Alloc>::MEMORY_HEAD;
    using typename CMemoryPoolBase<_Alloc>::PMEMORY_HEAD;
    using typename CMemoryPoolBase<_Alloc>::PLIST_NODE;
    using typename CMemoryPoolBase<_Alloc>::byte_pointer;

public:
    /**
     * @brief 构造一个空的内存池, 延迟分配内存
     */
    CMemoryObjectPool() : CMemoryPoolBase<_Alloc>(sizeof(value_type), 0) {}

    /**
     * @brief 带初始容量的构造函数
     * @param count 初始槽位数量
     */
    explicit CMemoryObjectPool(size_t count) : CMemoryPoolBase<_Alloc>(sizeof(value_type), count) {}

    /**
     * @brief 析构函数, 自动释放所有内存
     */
    ~CMemoryObjectPool() { CMemoryPoolBase<_Alloc>::destroy(); }

    /**
     * @brief 申请一个对象
     * @tparam _Args 构造函数的参数类型列表
     * @param ..._Arg 构造函数的参数, 无参时调用默认构造函数
     *        支持: pool.malloc() / pool.malloc(arg1) / pool.malloc(arg1, arg2) 等
     * @return 指向新分配对象的指针
     * @exception std::bad_alloc 分配失败时抛出
     */
    template<typename... _Args>
    inline pointer malloc(_Args&&... _Arg)
    {
        pointer p = static_cast<pointer>(CMemoryPoolBase<_Alloc>::alloc());
        construct(p, std::forward<_Args>(_Arg)...);
        return p;
    }

    /**
     * @brief 释放一个对象
     * @param p 指向要释放的对象指针, 不能为 nullptr, 必须是由本内存池分配的指针
     * @return true 释放成功; false 释放失败 (p 为 nullptr 或指针不属于本内存池)
     * @note 如果传入的地址未对齐到槽位边界:
     *       _DEBUG 模式触发 assert 断言失败
     *       release 模式自动对齐到正确的槽位地址
     */
    inline bool free(pointer p)
    {
        return CMemoryPoolBase<_Alloc>::free(p);
    }

    /**
     * @brief 将另一个池合并到本池
     * @param other 要合并进来的池,合并后会被清空
     * @exception std::runtime_error 传递进来的内存池非空时抛出
     * @note 合并后会按照块的尺寸排序
     */
    void merge(CMemoryObjectPool& other)
    {
        CMemoryPoolBase<_Alloc>::merge(other);
    }

    /**
     * @brief 将本池中所有空闲块分离到目标池
     * @param target 目标内存池,接收分离出来的空闲块
     * @return 分离出去的空闲块数量
     * @note 空闲块的判断标准: item 回到块起始位置 (bump pointer 已完全回退)
     *       分离后本池剩余块会保持原有顺序,目标池按尺寸排序
     */
    size_t split(CMemoryObjectPool& target)
    {
        return CMemoryPoolBase<_Alloc>::split(target);
    }

    /**
     * @brief 与另一个池交换所有内存块
     * @param other 要交换的池, 类型必须与本池一致
     * @exception std::runtime_error 传递进来的内存池是自身时抛出
     */
    void swap(CMemoryObjectPool& other)
    {
        if (&other == this)
            throw std::runtime_error("swap: 不能与自身交换");
        CMemoryPoolBase<_Alloc>::_swap(other);
    }

protected:
    void _before_free(void* p) override
    {
        _destroy_obj(reinterpret_cast<pointer>(p));
    }

    //------------------------------------------------------------
    // 销毁块内所有已分配对象
    //
    // 实现: SFINAE 编译期分支
    //   - trivially destructible: 空函数体,零开销
    //   - 否则:
    //       1. 计算 [pStart, item) 共有多少个槽位 N
    //       2. 分配位图:小块用栈上缓冲区;大块优先借用本块末尾空闲空间;空间仍不够才动态 new
    //       3. 遍历 free list,把对应 bit 置 1(表示已 free,不需析构)
    //       4. 遍历 [pStart, item),只对 bit = 0 的槽位调用析构函数
    //------------------------------------------------------------
    void _destroy_block(PMEMORY_HEAD pHead) override
    {
        _destroy_block_impl(pHead);
    }

private:

    //------------------------------------------------------------
    // 构造对象
    //
    // 实现: 使用 SFINAE 编译期分支
    //   - 有构造函数的类型: 调用 placement new, 参数完美转发
    //   - 无构造函数的类型 (int, double, PAINTSTRUCT 等): 空操作, 参数被忽略
    //------------------------------------------------------------
    template<typename... _Types>
    inline typename std::enable_if<sizeof...( _Types) == 0 || !std::is_trivially_default_constructible<_Ty>::value, void>::type
    construct(pointer _Ptr, _Types&&... _Args)
    {
        ::new (const_cast<void*>(static_cast<const volatile void*>(_Ptr))) _Ty(std::forward<_Types>(_Args)...);
    }

    template<typename... _Types>
    inline typename std::enable_if<sizeof...( _Types) != 0 && std::is_trivially_default_constructible<_Ty>::value, void>::type
    construct(pointer, _Types&&...) {}

    //------------------------------------------------------------
    // 析构单个对象
    //
    // 实现: 使用 SFINAE 编译期分支
    //   - 有析构函数的类型: 调用 p->~_Ty()
    //   - 无析构函数的类型 (int, double, PAINTSTRUCT 等): 空操作
    //------------------------------------------------------------
    template<int _IsTriviallyDestructible = std::is_trivially_destructible<_Ty>::value>
    inline typename std::enable_if<_IsTriviallyDestructible, void>::type
    _destroy_obj(pointer) {}

    template<int _IsTriviallyDestructible = std::is_trivially_destructible<_Ty>::value>
    inline typename std::enable_if<!_IsTriviallyDestructible, void>::type
    _destroy_obj(pointer p)
    {
        p->~_Ty();
    }

    //------------------------------------------------------------
    // 销毁一块内存块内的所有已分配对象 (内部实现)
    //------------------------------------------------------------
    template<int _IsTriviallyDestructible = std::is_trivially_destructible<_Ty>::value>
    inline typename std::enable_if<_IsTriviallyDestructible, void>::type
    _destroy_block_impl(PMEMORY_HEAD) {}

    template<int _IsTriviallyDestructible = std::is_trivially_destructible<_Ty>::value>
    inline typename std::enable_if<!_IsTriviallyDestructible, void>::type
    _destroy_block_impl(PMEMORY_HEAD pHead)
    {
        byte_pointer pStart = reinterpret_cast<byte_pointer>(pHead) + sizeof(MEMORY_HEAD);
        byte_pointer pEnd = pHead->item;

        size_t totalSlots = static_cast<size_t>(pEnd - pStart) / CMemoryPoolBase<_Alloc>::SLOT_SIZE;
        if (totalSlots == 0)
            return;

        // 如果回收链表是空的, 那就是 start 到 item 全部的槽位都要调用析构函数
        if (!pHead->freeList)
        {
            for (byte_pointer p = pStart; p < pEnd; p += CMemoryPoolBase<_Alloc>::SLOT_SIZE)
                _destroy_obj(reinterpret_cast<pointer>(p));
            return;
        }

        size_t bitmapWords = (totalSlots + 31) >> 5;

        // 栈上局部位图缓冲区,覆盖小块场景;大块优先借用本块末尾空闲空间;空间仍不够才动态 new
        static constexpr size_t STACK_BITMAP_SIZE = 0x800;
        uint32_t stackBitmap[STACK_BITMAP_SIZE];

        uint32_t* bitmap = nullptr;
        bool needFree = false;
        size_t bitmapBytes = bitmapWords * sizeof(uint32_t);
        if (bitmapWords <= STACK_BITMAP_SIZE)
        {
            bitmap = stackBitmap;
        }
        else
        {
            byte_pointer blockEnd = reinterpret_cast<byte_pointer>(pHead) + pHead->size;
            byte_pointer freeStart = pEnd;
            size_t freeBytes = static_cast<size_t>(blockEnd - freeStart);

            if (freeBytes >= bitmapBytes)
            {
                bitmap = reinterpret_cast<uint32_t*>(freeStart);
            }
            else
            {
                bitmap = reinterpret_cast<uint32_t*>(CMemoryPoolBase<_Alloc>::_Al.allocate(bitmapBytes));
                needFree = true;
            }
        }

        std::memset(bitmap, 0, bitmapBytes);

        // 遍历 free list,标记已释放的槽位
        PLIST_NODE pNode = pHead->freeList;
        while (pNode)
        {
            size_t idx = (reinterpret_cast<byte_pointer>(pNode) - pStart) / CMemoryPoolBase<_Alloc>::SLOT_SIZE;
            bitmap[idx >> 5] |= 1U << (idx & 31);
            pNode = pNode->next;
        }

        // 遍历所有槽位,对未标记的(活跃的)调用析构函数
        for (size_t i = 0; i < totalSlots; ++i)
        {
            if ((bitmap[i >> 5] & (1U << (i & 31))) == 0)
            {
                _destroy_obj(reinterpret_cast<pointer>(pStart + i * CMemoryPoolBase<_Alloc>::SLOT_SIZE));
            }
        }

        if (needFree)
            CMemoryPoolBase<_Alloc>::_Al.deallocate(reinterpret_cast<byte_pointer>(bitmap), bitmapBytes);
    }

};

// 字节池, 每次分配固定字节数的内存
// 槽位大小在运行时指定,已对齐到 sizeof(void*)
template<class _Alloc = CMemoryPoolAllocator<uint8_t>>
class CMemoryBytePool
    : public CMemoryPoolBase<_Alloc>
{
public:
    /**
     * @brief 构造字节池
     * @param slotSize 每次分配的字节数,会对齐到 sizeof(void*)
     * @param count 初始槽位数量,默认 4096
     */
    explicit CMemoryBytePool(size_t slotSize = sizeof(void*), size_t count = 0x1000)
        : CMemoryPoolBase<_Alloc>(slotSize, count) {}

    /**
     * @brief 申请一块内存
     * @return 指向新分配内存的指针
     * @exception std::bad_alloc 分配失败时抛出
     */
    void* malloc()
    {
        return CMemoryPoolBase<_Alloc>::alloc();
    }

    /**
     * @brief 释放一块内存
     * @param p 要释放的指针
     * @return true 释放成功; false 指针不属于本内存池
     */
    bool free(void* p)
    {
        return CMemoryPoolBase<_Alloc>::free(p);
    }

    /**
     * @brief 重新设置槽位尺寸
     * @param slotSize 新的槽位尺寸,已对齐到 sizeof(void*)
     * @exception std::runtime_error 如果池中仍有未释放的内存则抛出
     * @note 调用前必须确保池为空(所有块 item 回到起始位置),可用 is_empty() 检查
     */
    void resize_slot(size_t slotSize)
    {
        CMemoryPoolBase<_Alloc>::resize_slot(slotSize);
    }


    /**
     * @brief 将另一个池合并到本池
     * @param other 要合并进来的池,合并后会被清空
     * @exception std::runtime_error 传递进来的内存池非空时抛出
     * @note 合并后会按照块的尺寸排序
     */
    void merge(CMemoryBytePool& other)
    {
        CMemoryPoolBase<_Alloc>::merge(other);
    }

    /**
     * @brief 将本池中所有空闲块分离到目标池
     * @param target 目标内存池,接收分离出来的空闲块
     * @return 分离出去的空闲块数量
     * @note 空闲块的判断标准: item 回到块起始位置 (bump pointer 已完全回退)
     *       分离后本池剩余块会保持原有顺序,目标池按尺寸排序
     */
    size_t split(CMemoryBytePool& target)
    {
        return CMemoryPoolBase<_Alloc>::split(target);
    }

    /**
     * @brief 与另一个池交换所有内存块
     * @param other 要交换的池, 类型必须与本池一致
     * @exception std::runtime_error 传递进来的内存池是自身时抛出
     */
    void swap(CMemoryBytePool& other)
    {
        if (&other == this)
            throw std::runtime_error("swap: 不能与自身交换");
        CMemoryPoolBase<_Alloc>::_swap(other);
    }

};



NAMESPACE_MEMORYPOOL_END
