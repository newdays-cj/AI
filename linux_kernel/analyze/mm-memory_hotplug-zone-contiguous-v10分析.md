# mm/memory_hotplug: optimize zone contiguous check when changing pfn range (v10) 分析

## 一、基本信息

用户提供的 URL [^b4ty] 指向的是 maintainer David Hildenbrand 于 2026 年 9 月 23 日发出的 **b4-ty 应用通知**（b4 thank-you），不是补丁本身。该通知确认 Yuan Liu 提交的 v10 补丁系列已被合入 `mm/core.git` 仓库的 `memhp-7.4.misc` 分支，对应两个 commit：

- `[1/2] mm/memory_hotplug: make shrink_zone_span() more robust`，commit `1a3fed34`[^b4ty]
- `[2/2] mm/memory_hotplug: optimize zone contiguous check when changing pfn range`，commit `554e941f`[^b4ty]

通知原文明确说明：commit hash 可能因 rebase 或 trailer 更新而变化，且**不保证补丁最终进入主线**（"this is not a guarantee that the patches will go upstream"）[^b4ty]。维护者表示补丁将随后出现在 mm-next 和 linux-next 中。这能证明通知发出时已进入 MM 维护分支，不能证明截至分析时的主线状态；本次未直接核验当前分支或主线提交。

补丁系列基础 commit 为 `fd73f4a6659897191fa0d40695fe370925dd3780`，对应 Linux v7.3-rc3[^cover]。v10 相对 v9 的变化是 rebase 到 v7.3-rc3 并加入 Mike Rapoport 的 Acked-by[^cover]。

## 二、改动目的

当 `move_pfn_range_to_zone()` 或 `remove_pfn_range_from_zone()` 更新 zone 的 pfn 范围时，旧逻辑通过 `set_zone_contiguous()` 以 pageblock 粒度逐块扫描整个 zone 来重建 `zone->contiguous` 标志。对大型 zone 而言，这在内存热插拔和热拔过程中造成显著开销[^patch2]。

本系列引入一个新 zone 成员 `pages_with_online_memmap`，用 O(1) 的整数比较替代整 zone 逐 pageblock 扫描，优化热插拔性能[^cover][^patch2]。

## 三、解决的问题 / 达到的效果

### 3.1 性能提升

patch 2 给出了实测数据[^patch2]：

| 操作 | 大小 | 优化前 | 优化后 | 时间减少 |
|------|------|--------|--------|----------|
| 热插 | 256G | 10s | 3s | 70% |
| 热插 | 512G | 36s | 7s | 81% |
| 热拔 | 256G | 11s | 4s | 64% |
| 热拔 | 512G | 36s | 9s | 75% |

测试环境（作者声称）：Intel Icelake 服务器，KVM 加速，QEMU v9.0.0，客户机内核 v7.3-rc3，3 vCPU，初始内存 2G，使用 virtio-mem 设备[^patch2]。

需注意此处"时间"的定义：从输入 QEMU 热插拔命令到客户机中 `grep MemTotal /proc/meminfo` 反映出全部热插拔内存被识别的端到端耗时[^patch2]。这是手工操作到系统可见的完整链路测量，并非隔离的微基准测试。链路中包含 QEMU 设备处理、客户机内核 auto-online、buddy 分配器初始化等开销，zone 连续性检查仅是其中一环。作者未单独量化连续性检查环节占总耗时的比例。

cover letter 另声称本系列也惠及 CXL 热插拔，但定量数据指向一个独立链接[^cover]（`https://lore.kernel.org/all/20260409023552.GA2807@AE/`），本系列补丁本身未包含 CXL 的量化数据。

### 3.2 连续性检查更严格

新检查要求 `spanned_pages == pages_with_online_memmap`，即 zone 范围内每个 PFN 都满足 `pfn_to_online_page()`。旧逻辑以 pageblock 粒度通过 `pageblock_pfn_to_page()` 迭代，pageblock 内若存在 subsection 级别空洞仍可能被判定为连续。新检查粒度更细，不会漏掉 subsection 级空洞[^cover][^patch2]。

### 3.3 复杂度变化

连续性判定本身从 O(zone span / pageblock) 的全扫描降为 O(1) 的整数比较（基于代码推断）。这不意味着整个热插拔操作变成 O(1)：`shrink_zone_span()` 的边界搜索、`memmap_init_range()` 的初始化等工作仍然存在，补丁没有消除这些开销[^patch2]。

## 四、方案描述

### 4.1 新计数器 `pages_with_online_memmap`

在 `struct zone` 中新增成员 `pages_with_online_memmap`，语义为：zone 范围内拥有在线 memmap 的页面数，包括 present pages 以及 memmap 已初始化且 `pfn_to_online_page()` 成功的内存空洞[^cover][^patch2]。

该计数器与现有三个计数器的区别（基于代码 diff 和 `mmzone.h` 注释推断）[^mmzone][^patch2]：

- `spanned_pages`：zone pfn 跨度包含的全部页数，不论是否有效
- `present_pages`：物理存在的页数（含热插拔后的页），不包括仅 memmap 初始化的空洞页
- `managed_pages`：伙伴分配器可管理的页数（present 减去 reserved 等）
- `pages_with_online_memmap`：zone 范围内 memmap 在线且 `pfn_to_online_page()` 成功的页数，覆盖 present pages 和被初始化的在线空洞页

### 4.2 计数器初始化与更新路径

**启动阶段**（基于代码 diff 分析）[^patch2]：

- `memmap_init_zone_range()` 中 `memmap_init_range()` 初始化的 PFN 直接计入 `pages_with_online_memmap`（`zone->pages_with_online_memmap += end_pfn - start_pfn`）
- `init_unavailable_range()` 从 `void` 改为返回 `unsigned long`（在线页计数）。该函数处理 zone 范围内的空洞 PFN，通过 `CONFIG_SPARSEMEM_VMEMMAP` 条件下的 `pfn_section_valid()` 子区段检查，仅在线 subsection 中的页才计入 `online_pgcnt`
- 空洞 PFN 统计需排除 zone 范围外的已初始化页面：`memmap_init_zone_range()` 中若 `hole_start_pfn < zone_start_pfn`，先对 zone 外部分调用 `init_unavailable_range()`（返回值丢弃，不计入计数），再对 zone 内部分调用并累加计数

**热插拔阶段**[^patch2]：

- `adjust_present_page_count()` 中新增 `zone->pages_with_online_memmap += nr_pages`，与 `zone->present_pages += nr_pages` 同步。该函数在 memory online/offline 中被调用，`nr_pages` 可正可负
- 自托管 vmemmap（self-hosted vmemmap，即通过 altmap 在热插拔内存自身上存放 memmap）的增量与减量也经过 `adjust_present_page_count()`。在 `drivers/base/memory.c` 中，`memory_block_online` 对 `nr_vmemmap_pages` 调用 `adjust_present_page_count(pfn_to_page(start_pfn), mem->group, nr_vmemmap_pages)`，`memory_block_offline` 对应调用负值版本[^memory_c]。因此 `pages_with_online_memmap` 同时跟踪 vmemmap 页和常规页

### 4.3 连续性标志更新位置迁移

旧逻辑在 `move_pfn_range_to_zone()` 和 `remove_pfn_range_from_zone()` 内部直接调用 `clear_zone_contiguous()` / `set_zone_contiguous()`。新逻辑将这些调用从上述函数中删除，移至 `drivers/base/memory.c` 的 `memory_block_online()` / `memory_block_offline()` 中，包裹在 `mem_hotplug_begin()` / `mem_hotplug_done()` 之间[^memory_c][^patch2]。

**online 路径**（基于 diff）：

```
mem_hotplug_begin();
clear_zone_contiguous(zone);
... (mhp_init_memmap_on_memory, online_pages, adjust_present_page_count 等)
out:
set_zone_contiguous(zone);
mem_hotplug_done();
```

**offline 路径**（基于 diff）：

```
mem_hotplug_begin();
clear_zone_contiguous(mem->zone);
... (adjust, offline_pages, mhp_deinit 等)
out:
set_zone_contiguous(mem->zone);
if (!ret)
    mem->zone = NULL;
mem_hotplug_done();
```

关键设计点：

- 新 `set_zone_contiguous()` 仅在 `spanned_pages == pages_with_online_memmap` 时将 `contiguous` 写为 true，**不写 false**[^mmzone]。因此每次操作前必须先 `clear_zone_contiguous()` 置 false，操作完成后再由 `set_zone_contiguous()` 决定是否置 true。"先清除后设置"的模式是必需的
- offline 错误路径中，`mem->zone = NULL` 被延迟到 `set_zone_contiguous(mem->zone)` 之后，且仅在 `!ret` 时执行。这保证即使 offline 失败，`set_zone_contiguous(mem->zone)` 仍能在 zone 指针有效时执行标志恢复[^memory_c]
- online 初始化失败或 `online_pages()` 失败也经过共用 `out` 出口；offline 失败时，原有路径会回补先行扣除的自托管 vmemmap 页计数。最终重新判定依赖这些回滚语义，而非单靠指针有效。操作中的范围、计数和 memmap 状态更新必须在重新置 true 之前完成（代码推论）。

### 4.4 `set_zone_contiguous()` 重写

旧 `set_zone_contiguous()`（在 `mm/mm_init.c` 中的全 zone 扫描实现）被删除。新实现迁移至 `include/linux/mmzone.h` 中的 static inline 函数[^mmzone][^patch2]：

```c
static inline void set_zone_contiguous(struct zone *zone)
{
    if (zone_is_zone_device(zone))
        return;
    if (zone->spanned_pages == zone->pages_with_online_memmap)
        WRITE_ONCE(zone->contiguous, true);
}

static inline void clear_zone_contiguous(struct zone *zone)
{
    WRITE_ONCE(zone->contiguous, false);
}

static inline bool zone_is_contiguous(const struct zone *zone)
{
    return READ_ONCE(zone->contiguous);
}
```

ZONE_DEVICE zone 始终不会被标记为连续：`set_zone_contiguous()` 开头通过 `zone_is_zone_device(zone)` 直接 return[^mmzone]。

`READ_ONCE` / `WRITE_ONCE` 用于约束该标志的单次访问，不提供跨字段的 acquire/release 同步保证，也不能替代锁或 memmap 生命周期保护（代码推论）。`zone_is_contiguous()` 的注释明确警告：缺少与内存 offlining 的同步时，PFN 遍历仍存在竞态风险[^mmzone]。

## 五、关键实现细节

### 5.1 Patch 1: make shrink_zone_span() more robust

作者：David Hildenbrand (Arm)。此补丁独立于性能优化，修复 `shrink_zone_span()` 在 subsection 边界处的健壮性问题[^patch1]。

核心改动（基于 diff 分析）：

- 抽取 `subsection_overlaps_zone(pfn, zone)` 辅助函数：对给定 PFN 所在 subsection，先 `ALIGN_DOWN` 到 subsection 起始，检查 `pfn_to_online_page(start_pfn)` 是否在线（**offline 检查在前**），再检查 subsection 的**起始页或末尾页**是否属于目标 zone（`zone == page_zone(pfn_to_page(start_pfn)) || zone == page_zone(pfn_to_page(end_pfn))`）。补丁分别注明 subsection 的在线状态一致，以及 "Checking start+end is sufficient"。前者支撑在线性检查，后者依赖这里的 zone 边界布局，不能仅凭在线状态一致就推导出任意 zone 布局下检查两端都足够
- `find_smallest_section_pfn()` 和 `find_biggest_section_pfn()` 均改为调用该函数，去掉 `nid` 参数
- **去掉 `pfn_to_nid()` 检查**：补丁说明在 `SPARSEMEM_VMEMMAP_ENABLE` 下 `pfn_to_page()` 开销很低，而 `CONFIG_NUMA` 下 `pfn_to_nid()` 本身会调用 `pfn_to_page()`，先做 nid 检查没有意义[^patch1]
- `find_biggest_section_pfn()` 的反向循环从旧的 `pfn = end_pfn - 1; for (; pfn >= start_pfn; pfn -= PAGES_PER_SUBSECTION)` 改为 `for (; end_pfn > start_pfn; end_pfn -= PAGES_PER_SUBSECTION)` 并使用 `end_pfn - 1` 作为检查点。旧写法在 `pfn` 为 `unsigned long` 时，当 `start_pfn == 0` 会导致下溢（`0 - PAGES_PER_SUBSECTION` 回绕为巨大值），新写法以 `end_pfn > start_pfn` 为循环条件构成有界反向循环，消除了下溢风险

### 5.2 Patch 2: optimize zone contiguous check

作者：Yuan Liu，Co-developed-by: Tianyou Li。Acked-by: Mike Rapoport[^patch2]。

补充实现要点：

1. `page_alloc.h` 中 `pageblock_pfn_to_page()` 快速路径从直接读 `zone->contiguous` 改为调用 `zone_is_contiguous(zone)`[^patch2]
2. `remove_pfn_range_from_zone()` 和 `move_pfn_range_to_zone()` 中删除了 `clear/set_zone_contiguous()` 调用，连续性标志维护职责移交给 `memory_block_online/offline`[^patch2]
3. `Documentation/mm/physical_memory.rst` 新增 `pages_with_online_memmap` 条目，指向 `mmzone.h` 中的注释[^patch2]
4. `mm/mm_init.h` 中删除了旧 `set_zone_contiguous()` 声明和 `clear_zone_contiguous()` inline 定义，后者已迁移到 `mmzone.h`[^patch2]

Wei Yang 于 9 月 22 日给出 Reviewed-by，评价 "LGTM, thanks for the effort"[^review]。

## 六、方案的缺陷与使用限制

### 6.1 计数器可能安全欠计

补丁注释和 cover letter 均指出，`pages_with_online_memmap` 可能在 boot 后的热插拔过程中出现"过小"值[^cover][^patch2][^mmzone]。作者给出的示例（页面数仅作示意）：

- boot 后：spanned=10, initialized=15, online=10（online == spanned，判定连续）
- 热插 5 后：spanned=30, initialized=20, online=15（online != spanned，判定不连续）
- 热拔 5 后：spanned=15, initialized=15, online=10（online != spanned，判定不连续，但实际连续）

上述 "online" 即 `pages_with_online_memmap`，"initialized" 表示 memmap 已初始化的页数（含 zone 范围外的页）。boot 时 initialized(15) > spanned(10) 是因为部分已初始化页位于 zone 范围之外。热插后 zone 扩大覆盖了部分此类页，但 online 仍小于 spanned。热拔后 zone 缩小，online 回到 10 而 spanned 变为 15，导致 online < spanned 的欠计。

这种欠计仅影响连续性检测，使 zone 被保守地判为不连续。`pageblock_pfn_to_page()` 因而不能直接调用 `pfn_to_page(start_pfn)`，而需使用 `__pageblock_pfn_to_page()` 的检查路径；并不是恢复已删除的全 zone 连续性扫描。作者将此欠计描述为安全退化[^mmzone]。这依赖计数不发生高估，不能据此认为任意计数错误都安全。

### 6.2 更严格的检查改变现有行为

新检查要求每个 PFN 都满足 `pfn_to_online_page()`，比旧 pageblock 粒度更严格。对于以前在 pageblock 内有 subsection 空洞但被标记为连续的 zone，新逻辑会将其标记为不连续[^cover][^patch2]。这是行为变更，可能导致更多 zone 走慢路径。

### 6.3 无通用竞态修复

`zone_is_contiguous()` 注释明确警告 PFN 遍历仍可能与 offlining 产生竞态[^mmzone]。新增单次访问宏不构成新的锁或生命周期保护；本系列没有解决通用的并发下线遍历问题。

### 6.4 CXL 量化数据缺失

cover letter 声称惠及 CXL 热插拔，但定量数据指向独立链接[^cover]，本系列补丁未包含 CXL 相关量化性能数据。

### 6.5 OLK-6.6 兼容性未确认

补丁基于上游 v7.3-rc3[^cover]。本分析未验证其是否适用于 OLK-6.6，内核版本、代码结构和配置选项可能存在差异。无证据表明该补丁与 OLK-6.6 兼容。

### 6.6 合入通知不等于主线确认

b4-ty 通知明确说明 commit hash 可能因 rebase 或 trailer 更新而变化，不保证进入主线[^b4ty]。本次确认的是通知中的 MM 维护树 `memhp-7.4.misc` 分支合入记录，没有验证其后是否进入主线。

### 6.7 性能数据为端到端测量

如 3.1 节所述，性能数据为手工输入 QEMU 命令到 `grep MemTotal /proc/meminfo` 反映变化的端到端时间[^patch2]，非隔离基准测试。zone 连续性检查环节的单独贡献量未量化。

## 七、分析说明

本分析基于补丁 diff、cover letter、review 邮件及 b4-ty 通知的公开内容。文中标注"基于代码 diff 分析"或"基于代码与文档推断"的内容为从 diff 推导的逻辑结论，与作者声称的内容区分标注。本分析未进行构建或运行时基准测试。

cover letter 报告已测试 `online`/`online_movable` 策略、部分拔除、中间离线空洞、边缘 span 收缩、启动期对齐/非对齐及跨 zone subsection 空洞，以及 `kernelcore=mirror` 不高估场景。这些是作者报告的验证覆盖，不能等同于本次复现[^cover]。lore 与 git.kernel.org 页面在本次读取中触发 Anubis 验证；邮件内容通过下列公开镜像交叉核对。

## 参考来源

[^b4ty]: David Hildenbrand, "Re: [PATCH v10 0/2] mm/memory_hotplug: optimize zone contiguous check when changing pfn range" (b4-ty 应用通知), 2026-09-23. 原链接 <https://lore.kernel.org/all/179017451853.5582.14450577931941446538.b4-ty@b4/> ，可读镜像 <https://lists.openwall.net/linux-kernel/2026/09/23/2072>

[^cover]: Yuan Liu, "[PATCH v10 0/2] mm/memory_hotplug: optimize zone contiguous check when changing pfn range" (cover letter), 2026-09-20. <https://lkml.iu.edu/2609.2/11066.html>

[^patch1]: David Hildenbrand (via Yuan Liu), "[PATCH v10 1/2] mm/memory_hotplug: make shrink_zone_span() more robust", 2026-09-20. <https://lkml.iu.edu/2609.2/11067.html>

[^patch2]: Yuan Liu, "[PATCH v10 2/2] mm/memory_hotplug: optimize zone contiguous check when changing pfn range", 2026-09-20. <https://lkml.iu.edu/2609.2/11068.html>

[^review]: Wei Yang, "Re: [PATCH v10 2/2] mm/memory_hotplug: optimize zone contiguous check when changing pfn range", 2026-09-22. <https://lkml.iu.edu/2609.2/17284.html>

[^mmzone]: `include/linux/mmzone.h` diff, 见 [^patch2]

[^memory_c]: `drivers/base/memory.c` diff, 见 [^patch2]
