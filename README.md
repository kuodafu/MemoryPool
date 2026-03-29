# 扩大福内存池

轻量级、高效的定长内存池。

设计思路参考 [B站视频](https://www.bilibili.com/video/BV1ex421S79q/)

## 文件说明

| 文件 | 说明 | 状态 |
| --- | --- | --- |
| `CMemoryAllocator.h` | 底层内存分配器，接口兼容 `std::allocator` | ✅ 可用 |
| `CMemoryObjectPool.h` | 定长内存池，基于分配器实现 | ✅ 可用 |
| `CMemoryPool.h` | 变长内存池 | ⚠️ 暂不可用 |

## CMemoryObjectPool.h 核心设计

### 内存布局

```text
+---------------------------+ <- 按 0x1000 对齐（CPU 缓存行大小）
| MEMORY_HEAD               |  块头部信息
|---------------------------|
| 槽位 0 | 槽位 1 | ...     |  固定大小槽位
+---------------------------+
```

### CPU 缓存亲和

- **内存按 0x1000 对齐**，匹配 CPU 缓存行大小，减少缓存未命中
- **Bump 分配**，新分配的内存通常已在 CPU 缓存中
- **热点优先**，释放时先检查当前块，减少跨块访问
- **缓存友好遍历**，遍历其他块时优先 bump 分配（比 free list 更有缓存亲和）

### 分配策略

1. **Bump 分配** - 从当前块末尾顺序分配（最快路径）
2. **空闲链表复用** - 释放的槽位加入链表，优先复用
3. **块扩展** - 当前块不足时，申请新块（大小翻倍 + 1）

### 释放优化

- **Bump Pop** - 释放最后分配的槽位时，直接回退指针
- **整块重置** - 当整块内存都空闲时，自动重置为 bump 模式

## 基本用法

```cpp
#include "CMemoryObjectPool.h"

using Pool = kuodafu::CMemoryObjectPool<MyClass>;

// 创建内存池，初始槽位数
Pool pool(4096);

// 分配对象（支持构造函数参数）
auto* obj1 = pool.malloc();                    // 默认构造
auto* obj2 = pool.malloc(1, "hello");         // 带参构造

// 释放对象
pool.free(obj1);
pool.free(obj2);
```

## API 参考

| 方法 | 说明 |
| --- | --- |
| `init(count)` | 初始化内存池，设置初始槽位数 |
| `malloc(Args...)` | 分配一个对象，支持构造函数参数 |
| `free(ptr)` | 释放对象，自动调用析构函数 |
| `clear()` | 清空所有块，已分配对象调用析构函数 |
| `Release()` | 释放所有内存，已分配对象调用析构函数 |
| `query(ptr)` | 查询指针是否属于本内存池，debug 下断言地址对齐 |
| `dump()` | 输出调试信息 |

## 待实现功能

- [ ] 线程安全支持
- [ ] 作用域管理（RAII 自动回收）
- [ ] Debug 模式越界检测

## 开源协议

MIT License - 可自由使用，需署名，不承担使用风险

## 作者

由 [扩大福](https://github.com/kuodafu/) 设计实现
