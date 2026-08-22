# vLLM v1 KV Cache 与 Block 管理、分配逻辑详解

> 基于 vLLM v1 引擎源码深度分析
> 分析时间:2026-08-22

---

## 目录

1. [架构总览:分层结构](#一架构总览分层结构)
2. [配置与容量计算](#二配置与容量计算)
3. [Block 数据结构与 BlockPool 内部机制](#三block-数据结构与-blockpool-内部机制)
4. [分配主流程:allocate_slots](#四分配主流程allocate_slots)
5. [前缀缓存(Prefix Caching)](#五前缀缓存prefix-caching)
6. [调度器侧的 Block 生命周期](#六调度器侧的-block-生命周期)
7. [各注意力类型的管理器差异](#七各注意力类型的管理器差异)

---

## 一、架构总览:分层结构

KV cache 管理采用**五层**设计,上层做决策、下层做物理管理:

```
EngineCore / Scheduler             ← 决策层: 每步为每个请求调用 allocate_slots
        │
KVCacheManager                     ← 门面层: watermark、失败门、CoW 记账
        │  (vllm/v1/core/kv_cache_manager.py)
        ▼
KVCacheCoordinator                 ← 协调层: 多 group 的块数汇总与定点迭代
        │  (vllm/v1/core/kv_cache_coordinator.py)
        │  ├─ NoPrefixCache / Unitary(单组) / Hybrid(多组)
        ▼
SingleTypeKVCacheManager × N       ← 每类注意力各一个: 维护每请求块表
        │  (vllm/v1/core/single_type_kv_cache_manager.py)
        │  ├─ FullAttentionManager / SlidingWindowManager
        │  ├─ MambaManager / CrossAttentionManager 等
        ▼
BlockPool(唯一,全局共享)           ← 物理层: 空闲链表 + 哈希表
        │  (vllm/v1/core/block_pool.py)
        ▼
KVCacheBlock × num_blocks          ← 物理块元数据(kv_cache_utils.py)
```

**关键设计**:所有注意力类型共享**同一个 BlockPool**(`kv_cache_coordinator.py:98`),block id 全局统一编号;每个 group 通过各自的 `SingleTypeKVCacheManager` 维护"请求 → block 列表"的映射(`req_to_blocks`),但底层 block 来自同一个池。

---

## 二、配置与容量计算

### 2.1 KVCacheSpec 谱系与 page_size_bytes 公式

文件:`vllm/v1/kv_cache_interface.py`

- `KVCacheSpec` 基类(141-214):唯一必填字段 `block_size`(一个 block 的 token 数)。
- `AttentionSpec`(217-271)字段:`num_kv_heads`、`head_size`、`dtype`、`head_size_v`(=head_size)、`kv_quant_mode`、`page_size_padded` 等。

**page_size_bytes 精确公式**(一个物理 block 占用的字节数):

```
state_content_size_bytes    = (head_size + head_size_v) * dtype_size      # K+V 同维
unpadded_page_size_bytes    = num_heads * storage_block_size * state_content_size_bytes
page_size_bytes             = page_size_padded(若设置) 否则 unpadded
```

以 FlashAttention 为例:物理张量形状 `(num_blocks, num_kv_heads, block_size, 2*head_size)`,即 `page_size_bytes = num_kv_heads × block_size × 2 × head_size × dtype_size`。

各类 spec 的 `max_memory_usage_bytes`(容量上限估算):
- `FullAttentionSpec`:`cdiv(max_model_len/dcp, block_size) * page_size_bytes`
- `SlidingWindowSpec`:`cdiv(min(sliding_window-1+extra_retained+max_in_flight_tokens, max_model_len), block_size) + 1` 块
- `MambaSpec`:`sum(prod(shape) * dtype_size)`(状态张量而非 K/V 页)
- `CrossAttentionSpec`:按 `max_num_encoder_input_tokens` 计
- `UniformTypeKVCacheSpecs`(混合 hidden size,如 DeepSeek V4):`page_size_bytes = Σ 各层`

### 2.2 block_size 双层机制

**两层 block_size 解耦**:

| 层 | 含义 | 来源 |
|---|---|---|
| KV 管理器 block_size | 调度/哈希粒度,决定块表 | `cache_config.block_size`;sliding window 层**自选**(`attention.py:597` 的 `_largest_kernel_block_within`) |
| kernel block size | 注意力内核实际块大小 | 后端 `get_supported_kernel_block_sizes()`:FlashAttn `[MultipleOf(16)]`,FlashInfer `[16,32,64]` |

运行时由 `select_common_block_size()`(`worker/utils.py:301`)为每个 group 选一个所有后端都支持、且能整除管理器 block_size 的 kernel block size。

### 2.3 scheduler_block_size(LCM)

`resolve_kv_cache_block_sizes()`(`kv_cache_utils.py:648`):

- 单 group:`scheduler_block_size = block_size × dcp`
- 多 group(hybrid):`scheduler_block_size = math.lcm(*group_block_sizes)`——注意 Attention 组乘 dcp、**Mamba 组不乘**(state 全秩复制不 shard)
- `hash_block_size`:prefix caching 启用时取 `gcd` 或 `prefix_match_unit`,且必须整除所有 group 的 block_size

`scheduler_block_size` 是 token 对齐不变量:协调器断言 `scheduler_block_size % hash_block_size == 0` 且整除每个 group block_size(`kv_cache_coordinator.py:91-94`)。

### 2.4 内存 Profiling 与 num_blocks 派生

**请求内存上界**(`worker/utils.py:444`):
```
requested_memory = ceil(total_memory × gpu_memory_utilization)
```

**Profiling 主流程**(`gpu_worker.py:475-626`):
1. 配置了 `kv_cache_memory_bytes` → 跳过 profiling 直接使用
2. 否则在 `memory_profiling` 上下文跑 `profile_run()`(dummy 输入,`skip_attn=True`,只测权重+激活峰值)
3. 可选:估算 CUDA graph 内存(`VLLM_MEMORY_PROFILER_ESTIMATE_CUDAGRAPHS=1`)

**Profiling 数学**(`mem_utils.py:234-326`):
```
total_consumed         = before.free_memory - after_profile.free_memory   # mem_get_info,非 torch 口径
transient_peak_headroom = after.torch_peak - after.torch_allocated        # 峰值激活余量
non_kv_cache_memory     = total_consumed + transient_peak_headroom
available_kv_cache_memory = requested_memory - non_kv_cache_memory - cudagraph_estimate
```

**安全余量**:无固定百分比;余量来自 ①`non_kv_cache_memory` 含权重+激活峰值+非 torch 内存全部扣除;②额外预留 1 个 null block(`get_kv_cache_configs` 中 `check_memory = avail_mem - _pool_bytes_per_block`)。

**num_blocks 三条派生路径**(`kv_cache_utils.py:1377-1465`):
1. 单 group + 混合 hidden size:`num_blocks = available_memory // group.page_size_bytes`
2. packed 布局(DeepSeek V4):`num_blocks = available_memory // block_stride`
3. 一般情况(经典公式):**`num_blocks = int(available_memory // page_size // num_layers)`**

多 worker 时取所有 rank 的最小值并按比例缩放张量。

---

## 三、Block 数据结构与 BlockPool 内部机制

### 3.1 KVCacheBlock(物理块元数据)

文件:`vllm/v1/core/kv_cache_utils.py:159-218`

| 字段 | 含义 |
|---|---|
| `block_id: int` | 0 … num_blocks-1 |
| `ref_cnt: int` | 引用计数 |
| `_block_hash` | 满块/部分块的哈希键(仅缓存时有值) |
| `_block_hash_num_tokens` | 哈希覆盖的前缀 token 数 |
| `prev/next_free_block` | 空闲链表指针(仅被 FreeKVCacheBlockQueue 操作) |
| `is_null` | null block 标记,永不被缓存 |

### 3.2 FreeKVCacheBlockQueue:零分配双向链表

文件:`kv_cache_utils.py:226-469`

**不分配 Python 对象**——直接改写 block 上的指针;假头/假尾(block_id=-1)消除空指针分支。

**驱逐顺序不变式**(类 docstring 235-241,至关重要):
```
1. 队首 = 最久未使用(LRU,最先被驱逐)
2. 同时被释放时,链尾(哈希覆盖更多 token 的块)排更前
   —— 这个"释放时反转顺序"由调用方(kv_cache_manager)完成
```

操作:`popleft` / `popleft_n`(从队首取)、`remove`(O(1) 中删,`touch` 用)、`append`(队尾,最不活跃)、`prepend_n`(队首插,保序)、`append_n`(队尾插,保序)。

### 3.3 BlockHashToBlockMap:哈希索引

文件:`block_pool.py:33-140`

`_cache: dict[BlockHashWithGroupId, KVCacheBlock | dict[int, KVCacheBlock]]`——单块用值、多块升级为 dict(避免内部 dict GC 开销)。

**关键设计:不做去重**——相同内容的块共存,因为 block id 必须只追加、永不变。

### 3.4 BlockPool 构造与 null block

`block_pool.py:162-196`:
```python
self.blocks = [KVCacheBlock(idx) for idx in range(num_gpu_blocks)]
self.free_block_queue = FreeKVCacheBlockQueue(self.blocks)
self.null_block = self.free_block_queue.popleft()   # 永远是 block_id == 0
self.null_block.is_null = True                       # ref_cnt 恒为 0,永不被 free
```

**null block 的作用**:注意力内核按 block table / slot_mapping 索引 KV 页,未使用/填充槽位指向 `NULL_BLOCK_ID = 0`(`attention/backends/utils.py:46`),保证读到合法零块而非越界内存。它被排除在缓存(`cache_full_blocks` 跳过 `is_null`)、释放、`get_usage`(减 1)、`reset_prefix_cache`(期望恰好 1 块被占用)之外。

### 3.5 Block 生命周期:每个操作的精确状态转换

| 操作 | 空闲链表 | ref_cnt | 哈希 |
|---|---|---|---|
| **分配** `get_new_blocks`(647-677) | `popleft_n` 摘下 | 断言为 0 → 1 | 若有残留哈希先驱逐(`_maybe_evict_cached_block`) |
| **命中** `touch`(702-717) | `ref_cnt==0` 时 `remove` 摘出(重新锚定) | +1 | 不变 |
| **缓存** `cache_full_blocks`/`cache_partial_block` | 不变 | 不变 | `_insert_block_hash` 设置主哈希/登记次哈希 |
| **释放** `free_blocks`(719-743) | ref_cnt 归 0 时入链表 | −1 | 不变(缓存块哈希保留,供未来命中) |
| **驱逐(哈希)** `_maybe_evict_cached_block` | 不变 | 不变 | `_remove_cached_block_hashes` 清空 |

**不变式**:块在空闲链表中 ⟺ `ref_cnt == 0`(null block 除外)。

### 3.6 释放时的 LIFO/FIFO 双轨制(核心精妙设计)

`free_blocks`(`block_pool.py:719-743`):
```python
# 未缓存块(无哈希)→ prepend_n → LIFO:立即复用,GUI 局部性
# 缓存块(有哈希)→ append_n → FIFO:最后复用,LRU 驱逐
```
**为什么**:未缓存块没有前缀价值,立即复用(队首)带来最好的 GPU 内存局部性;缓存块是可复用的前缀数据,放到队尾实现 LRU 语义,尽量延长其在池中的存活时间。

### 3.7 驱逐的两层语义

1. **前缀缓存驱逐**(清哈希元数据):`get_new_blocks` 分配时对刚弹出的块调用 `_maybe_evict_cached_block`;或显式 `evict_blocks(block_ids)`(KV 连接器远端驱逐通知)。`ref_cnt>0` 的块只被从哈希表驱逐、**不释放**。
2. **物理复用**:空闲不足时 `popleft_n` 从队首(LRU)取块。候选 = 所有 `ref_cnt==0` 的块,选择依据 = 链表位置(由 3.6 的双轨制和 `touch` 维护)。

---

## 四、分配主流程:allocate_slots

文件:`vllm/v1/core/kv_cache_manager.py:347-568`

### 4.1 参数语义

```
comp       = request.num_computed_tokens           # 已计算
new_comp   = num_new_computed_tokens               # 本步本地 prefix cache 命中
ext_comp   = num_external_computed_tokens          # 连接器缓存的 token(P/D 分离)
new        = num_new_tokens                        # 本步要新算的(含推测草稿)
lookahead  = num_lookahead_tokens                  # EAGLE 等前瞻
delay_cache_blocks → 跳过缓存提交(P/D 异步传输)
reserved_blocks → 为其他在途序列预留的空闲块
has_scheduled_reqs → 本步是否已有请求被调度(控制 watermark)
```

### 4.2 八步流程

```
1. 计算 watermark_blocks
2. [可选] full_sequence_must_fit 门:整条序列放不下 → return None
3. remove_skipped_blocks:释放滑窗外的块(按 processed token 计,
   使在途步骤仍可读、被拒绝的草稿 token 可回滚边界)
4. get_num_blocks_to_allocate:Σ 各 manager 所需块数
5. 失败门: required_blocks + watermark > free - reserved → return None
6. allocate_new_computed_blocks(两阶段):
   a. add_local_computed_blocks:touch 命中块 + null 块填充跳过位
   b. allocate_external_computed_blocks(仅 ext_comp>0 时)
7. allocate_new_blocks:先 CoW 重定向,再取新块
8. cache_blocks:把新满块提交进前缀缓存
   num_tokens_to_cache = min(total_computed + new, request.num_tokens)
   (截断到 num_tokens,排除可能被拒绝的草稿 token)
```

**`return None` 是唯一的失败信号**——调度器必须抢占或拒绝接纳该请求。

### 4.3 Watermark 水位线

```python
watermark_blocks = int(watermark * num_blocks)                    # 默认 0
# 仅当 has_scheduled_reqs 且 status ∈ {WAITING, PREEMPTED} 时生效
required_blocks = num_blocks_to_allocate + watermark_blocks
```
**语义**:接纳等待/被抢占请求时保留 headroom,避免频繁抢占。RUNNING 请求豁免。与 `reserved_blocks`(从空闲数中减)是正交的两个旋钮。

### 4.4 块数公式

```python
num_required_blocks = cdiv(num_tokens, block_size)     # cdiv = -(a // -b),向上取整
num_new_blocks = max(num_required_blocks - max(num_skipped_blocks, num_local_computed_blocks), 0)
+ num_evictable_blocks   # 将被 touch 的 ref_cnt==0 命中块也要计入(会离开空闲链表)
+ 1                      # 部分命中时 CoW 预留
```

### 4.5 Copy-on-Write(部分命中时的写时复制)

**触发条件** `_has_partial_local_hit`(129-139):`len(new_computed_blocks) > 0` 且 `num_local_computed_tokens % block_size != 0`——即命中终点落在块**内部**,共享尾块不能安全原地续写。

**流程**:
1. `add_local_computed_blocks` 记录 `_partial_hit_reqs[req_id] = (block_idx, 尾块)`,并封顶 `num_cached_block = block_idx`(280-286)
2. `get_num_blocks_to_allocate` 预留 +1 块(223-226)
3. `allocate_new_blocks` 弹出记录,取新块,`_apply_cow`:
   ```python
   req_blocks[block_idx] = cow_block          # 块表指向私有副本
   _pending_cow_copies.append((source, cow))  # 拷贝任务排队
   cow_block.ref_cnt += 1                     # 拷贝完成前保持引用
   ```
4. 拷贝本身在 worker 上执行(`take_kv_cache_block_copies` 取出)
5. Mamba "align" 变体:运行中请求块表只追加,改为 `move_block_hashes(source, cow)`——请求继续读 source,拷贝填 cow

### 4.6 num_cached_block vs req_to_blocks

| | `req_to_blocks[req_id]` | `num_cached_block[req_id]` |
|---|---|---|
| 含义 | 完整块表(分配顺序) | 已提交前缀缓存的**前导块计数** |
| 内容 | 含 null 填充 + 新分配未缓存块 | 纯计数,是 `cache_blocks` 的续写游标 |
| 生命周期 | 跨抢占持久 | 仅跟踪 RUNNING 请求 |

---

## 五、前缀缓存(Prefix Caching)

### 5.1 链式哈希

```python
block_hash = hash(父块哈希, 本块token元组, extra_keys)   # kv_cache_utils.py:618
# 首块父哈希 = NONE_HASH;extra_keys = LoRA名 + 多模态特征 + cache_salt + prompt embeds
```
每块哈希天然指纹化其**全部前缀**,使"找最长命中"退化为逐块查表。哈希粒度 `hash_block_size` ≤ 管理器 block_size,通过 `BlockHashListWithBlockSize` / `resolve_block_hashes` 协调,支持**细粒度块内命中**(fine-grained lookup)。

### 5.2 查找:find_longest_cache_hit

`max_cache_hit_length = request.num_tokens - 1`(最后 1 token 必须重算拿 logits)。

- **FullAttentionManager**(683-779):Phase 1 整块前向走(链式哈希使首 miss 即停)→ Phase 2 块内高到低探测;EAGLE 时丢弃末块。
- **SlidingWindowManager**(902-999):断言无细粒度;**右到左扫描**连续缓存块,凑够 `cdiv(window_size-1, block_size)` 块即命中。
- **MambaManager**(1294-1371):细粒度或粗粒度右到左;前导 null 填充。
- **HybridCoordinator**(757-889):**定点迭代**——各组轮流查找,每轮 `hit_length` 单调递减,收敛(长度不再变化)或 simple-hybrid(全注意力 + 单杂项组)一轮即停。

### 5.3 缓存回写

- `cache_full_blocks`:新满块 → `_insert_block_hash` → 设主哈希 + 登记进 `cached_block_hash_to_block`
- `cache_partial_block`:prompt 终点落在块内时,登记**部分块条目**(`block_hash_num_tokens` 记录覆盖前缀长度);部分 → 满时先清旧哈希再升级

### 5.4 reset_prefix_cache

`block_pool.py:764-798`:仅当 `num_used_blocks == 1`(只剩 null block)时成功;清空哈希表、`reset_hash()` 所有块。

---

## 六、调度器侧的 Block 生命周期

文件:`vllm/v1/core/sched/scheduler.py`

### 6.1 两个 allocate_slots 调用点

| 阶段 | 位置 | 参数 | 失败语义 |
|---|---|---|---|
| Phase 1 RUNNING | 639-643 | `num_new_tokens, num_lookahead_tokens` | `None` → **抢占**受害者重试 |
| Phase 2 WAITING | 1041-1053 | 全量参数(命中块、外部块、delay_cache_blocks、reserved_blocks…) | `None` → **break**,拒绝接纳,不抢占 |

前缀缓存查找仅在 `request.num_computed_tokens == 0` 时进行(819-826),经 `_get_local_prefix_cache_hit` 分派到 `get_computed_blocks`(或连接器版)。

### 6.2 抢占的精确顺序(_preempt_request,1347-1388)

```python
1. _free_request_blocks(request)      # 先释放 KV 块
2. encoder_cache_manager.free(request)
3. _inflight_prefills.discard(request)
4. status = PREEMPTED; num_computed_tokens = 0   # 清进度 → 重新入队后重算
5. spec_token_ids = []; num_preemptions += 1
6. waiting.prepend_request(request)   # 回等待队列头部
```
受害者选择(649-696):PRIORITY 取 `max(priority, arrival_time)`,FCFS 取 `running.pop()`(最新)。恢复时块表是**替换**而非追加(`CachedRequestData.resumed_req_ids`)。

### 6.3 延迟释放围栏(Deferred Free Fence)

**仅在 `max_concurrent_batches > 1` 且 KV consumer 连接器时启用**(159-165)——重叠批次下,在途 GPU 步骤可能仍在写被释放请求的 KV 块。

```
sched_step_seq += 1         # schedule() 末尾,仅非空步(1321-1322)
processed_step_seq += 1     # update_from_output() 开头,仅非空步(1759-1763)
                           # 此时该步及之前的所有 GPU 写已确认完成
```

`_free_request_blocks`(2463-2476)的围栏决策:
```python
if not defer_block_free or request.last_sched_seq <= processed_step_seq:
    kv_cache_manager.free(request)          # 立即释放
else:
    blocks = pop_blocks_for_free(request)   # 摘出块表,块对象入队
    deferred_frees.append((sched_step_seq, blocks))
```
`_drain_deferred_frees`(2489-2502):队首围栏 ≤ `processed_step_seq` 则 `free_blocks(reversed(blocks))`(反序,尾块先驱逐)。

### 6.4 完成与中止

- 正常完成:`update_from_output` **先**推进 `processed_step_seq` 并排空延迟队列,**后**调用 `_free_request`(1997-2003)——因此正常完成总是立即释放(`last_sched_seq <= processed_step_seq` 恒成立)
- 流式/可恢复请求:`_handle_stopped_request` 返回 False,**不释放**
- 中止:`abort_requests → finish_requests`(2359-2420)两遍处理;`WAITING_FOR_REMOTE_KVS` 且未收到 KV 的请求 `delay_free_blocks=True`;`_process_aborts_queue` 在 `update_from_output` **之前**处理(core.py:707-712),被中止请求的迟到输出由 `is_finished()` 守卫跳过

### 6.5 推测解码拒绝回滚(1844-1865)

```python
num_rejected = num_draft_tokens - num_accepted
request.num_computed_tokens -= num_rejected        # 回滚进度
request.num_output_placeholders -= num_rejected    # 异步调度下同步回滚
```
已分配给被拒位置的 KV 块**不释放**——`num_computed_tokens` 回退后,下一步 Phase 1 重算 `num_new_tokens` 会重新推导这些位置并覆写。`output_is_stale` 守卫防止抢占后的陈旧拒绝计数二次生效。

### 6.6 reset_prefix_cache(2545-2585)

```python
if reset_running_requests:
    while self.running:
        self._preempt_request(running.pop(), drop_stale_output=True)  # 全部抢占
    prev_step_scheduled_req_ids.clear()
reset_successful = kv_cache_manager.reset_prefix_cache()   # 仅剩 null block 才成功
```
- 运行中请求全部被抢占(释放块 → 引用清零 → reset 可成功),`drop_stale_output=True` 丢弃在途输出防止乱序
- `reset_running_requests=True` 时失败抛 `RuntimeError`(如 `WAITING_FOR_REMOTE_KVS` 无法抢占)
- WAITING/PREEMPTED 请求不持块,不受直接影响

---

## 七、各注意力类型的管理器差异

| 管理器 | get_num_skipped_tokens | 命中查找 | 特点 |
|---|---|---|---|
| **FullAttentionManager** | 0(永不提前释放) | 两阶段(整块+细粒度) | `get_num_common_prefix_blocks` 用 `ref_cnt == len(req_to_blocks)` |
| **SlidingWindowManager** | `max(0, computed - window + 1 - extra)` | 右到左连续扫描 | 跳过的块 null 填充,可提前释放窗外块 |
| **ChunkedLocalAttentionManager** | `(computed // chunk) * chunk` | 块内前向 | chunk 外 null 填充 |
| **MambaManager** | `computed - 1`(只需最后 token 状态) | 右到左/细粒度 | "align" 模式:块表只追加 + `move_block_hashes` CoW 变体 + `last_state_block_idx` |
| **CrossAttentionManager** | — | NotImplementedError | 纯 `allocate_new_blocks`,按 encoder token 分配,不缓存 |
| **RSWAManager** | — | — | 间隙驱逐(remove_skipped_blocks with gap eviction) |

---

## 八、端到端分配调用序(Happy Path)

```
Scheduler.schedule()
  ├─ Phase 1(RUNNING):  allocate_slots(req, num_new_tokens, lookahead)  [失败→抢占]
  ├─ Phase 2(WAITING):  get_computed_blocks → 命中块+命中token数
  │                      allocate_slots(req, num_new, new_comp, 命中块, ext_comp, ...)
  │
allocate_slots 内部:
  1. watermark(仅 WAITING/PREEMPTED)
  2. [可选] full_sequence_must_fit 门
  3. remove_skipped_blocks(窗外块释放)
  4. get_num_blocks_to_allocate = Σ cdiv(num_tokens, block_size) 调整
  5. 失败门: 所需+水位 > 空闲-预留 → None
  6. 两阶段: touch 命中块 → 分配外部块
  7. allocate_new_blocks: CoW 重定向 → get_new_blocks
  8. cache_blocks: min(total+new, num_tokens) 提交缓存
  → KVCacheBlocks(block_ids) → SchedulerOutput → worker BlockTables
```

---

## 附录:关键文件索引

| 文件 | 核心内容 |
|---|---|
| `vllm/v1/core/kv_cache_manager.py` | `allocate_slots`(347)、`get_computed_blocks`(232)、`free`(570) |
| `vllm/v1/core/kv_cache_coordinator.py` | 协调器 ABC + 三实现、Hybrid 定点迭代(757) |
| `vllm/v1/core/single_type_kv_cache_manager.py` | 各注意力管理器、块表 `req_to_blocks`(94) |
| `vllm/v1/core/block_pool.py` | `BlockPool`(143)、`free_blocks` 双轨制(719)、null block(190) |
| `vllm/v1/core/kv_cache_utils.py` | `KVCacheBlock`(159)、空闲链表(226)、链式哈希(618) |
| `vllm/v1/kv_cache_interface.py` | `KVCacheSpec`/`KVCacheConfig` 谱系 |
| `vllm/v1/core/sched/scheduler.py` | 抢占(1347)、延迟释放围栏(2463/2489)、reset(2545) |
| `vllm/v1/worker/gpu_worker.py` | 内存 profiling(475) |
| `vllm/utils/mem_utils.py` | profiling 数学(234) |
| `vllm/v1/worker/gpu/attn_utils.py` | KV 张量分配与 reshape |
