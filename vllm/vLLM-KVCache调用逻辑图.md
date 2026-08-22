# vLLM KVCache-Block 关键代码调用逻辑图

> 基于 vLLM v1 源码绘制的关键调用链(含文件与行号)
> 配套文档:`vLLM-KVCache-Block管理分配逻辑详解.md`

---

## 图 1:一次调度步骤的全局调用链(时序图)

```mermaid
sequenceDiagram
    autonumber
    participant EC as EngineCore.step<br/>(engine/core.py:583)
    participant SC as Scheduler.schedule<br/>(sched/scheduler.py:484)
    participant KM as KVCacheManager<br/>(kv_cache_manager.py)
    participant CO as KVCacheCoordinator<br/>(kv_cache_coordinator.py)
    participant ST as SingleTypeKVCacheManager<br/>(single_type_kv_cache_manager.py)
    participant BP as BlockPool<br/>(block_pool.py)

    EC->>SC: schedule()
    SC->>KM: new_step_starts() [523]

    Note over SC: Phase 1: RUNNING 请求补 token
    SC->>KM: allocate_slots(req, num_new_tokens, lookahead) [639]
    KM->>CO: remove_skipped_blocks / get_num_blocks_to_allocate
    CO->>ST: 按组汇总所需块数
    KM->>CO: allocate_new_blocks
    CO->>ST: allocate_new_blocks
    ST->>BP: get_new_blocks(n) → popleft_n [661]
    KM->>CO: cache_blocks [566]
    ST->>BP: cache_full_blocks
    alt 分配失败(None)
        KM-->>SC: return None
        SC->>SC: _preempt_request(受害者) [1347] → 重试
    end

    Note over SC: Phase 2: WAITING 请求接纳(仅 num_computed_tokens==0 时查前缀缓存)
    SC->>KM: get_computed_blocks(request) [826]
    KM->>CO: find_longest_cache_hit(block_hashes, max_hit_len)
    CO->>BP: get_cached_block(block_hash) × 逐块
    BP-->>CO: 命中块列表
    KM-->>SC: (命中块, num_new_computed_tokens, shared_prefix_boundary)
    SC->>KM: allocate_slots(全量参数) [1041]
    KM->>CO: allocate_new_computed_blocks(两阶段)
    CO->>ST: add_local_computed_blocks → touch(命中块)
    CO->>ST: allocate_external_computed_blocks(若 ext_comp>0)
    CO->>ST: allocate_new_blocks(CoW 重定向 + 新块)
    KM->>CO: cache_blocks [566]
    SC-->>EC: SchedulerOutput(block_ids)
```

---

## 图 2:allocate_slots 内部八步流程(流程图)

```mermaid
flowchart TD
    A[allocate_slots<br/>kv_cache_manager.py:347] --> B{1. 计算 watermark_blocks<br/>仅 WAITING/PREEMPTED 且<br/>has_scheduled_reqs:466-473}
    B --> C{2. full_sequence_must_fit?<br/>:475-491}
    C -->|是 且整序列放不下| X[return None → 调度器拒绝/抢占]
    C -->|否 或放得下| D[3. remove_skipped_blocks<br/>释放滑窗外块:507-511]
    D --> E[4. get_num_blocks_to_allocate<br/>Σ cdiv num_tokens, block_size:513-522]
    E --> F{5. 失败门<br/>所需+watermark > 空闲-reserved?<br/>:524-530}
    F -->|是| X
    F -->|否| G[6a. add_local_computed_blocks<br/>touch 命中块 + null 填充:253-259]
    G --> H{有外部计算 token?}
    H -->|是| I[6b. allocate_external_computed_blocks<br/>:260-266]
    H -->|否| J[7. allocate_new_blocks<br/>先 CoW 重定向再取新块:545-550]
    I --> J
    J --> K{8. delay_cache_blocks<br/>或缓存关闭? :552-555}
    K -->|是| L[返回块,不提交缓存<br/>P/D 异步传输场景]
    K -->|否| M[cache_blocks<br/>num_tokens_to_cache =<br/>min total+new, num_tokens:557-566]
    M --> N[返回 KVCacheBlocks<br/>→ SchedulerOutput.block_ids]
```

---

## 图 3:前缀缓存查找(时序图)

```mermaid
sequenceDiagram
    autonumber
    participant SC as Scheduler
    participant KM as KVCacheManager.get_computed_blocks<br/>(kv_cache_manager.py:232)
    participant CO as KVCacheCoordinator.find_longest_cache_hit
    participant ST as FullAttentionManager<br/>(single_type_kv_cache_manager.py:683)
    participant BP as BlockPool

    SC->>KM: get_computed_blocks(request)
    KM->>KM: max_cache_hit_length =<br/>request.num_tokens - 1 [262]
    Note over KM: -1: 最后 1 token 必须重算拿 logits
    KM->>CO: find_longest_cache_hit(block_hashes, max_hit_length)
    alt Unitary(单组)
        CO->>ST: find_longest_cache_hit
    else Hybrid(多组)
        CO->>CO: 定点迭代: 各组轮流查找<br/>hit_length 单调递减至收敛 [757-889]
        CO->>ST: 每组 find_longest_cache_hit
    end
    ST->>ST: resolve_block_hashes<br/>哈希粒度 → 块粒度
    rect rgb(230, 240, 250)
        Note over ST,BP: Phase 1 整块前向走
        loop 逐块(链式哈希: 首 miss 即停)
            ST->>BP: get_cached_block(block_hash, group_ids)
            alt 命中
                BP-->>ST: KVCacheBlock
            else miss
                BP-->>ST: None → 停止
            end
        end
        Note over ST,BP: Phase 2 细粒度块内探测(可选)
        ST->>BP: get_cached_block(块内 hash 边界, 高→低)
    end
    ST-->>CO: (命中块列表, hit_length)
    CO-->>KM: (blocks, num_new_computed_tokens, num_uncached)
    KM->>KM: shared_prefix_boundary =<br/>hit + uncached(若有)[293-295]
    KM-->>SC: (KVCacheBlocks, hit_tokens, boundary)
```

---

## 图 4:部分命中 Copy-on-Write(时序图)

```mermaid
sequenceDiagram
    autonumber
    participant ST as SingleTypeKVCacheManager
    participant BP as BlockPool
    participant WK as Worker(拷贝执行)

    Note over ST: 触发: _has_partial_local_hit<br/>num_local_computed % block_size != 0 [129-139]<br/>命中终点落在块内部 → 共享尾块不能原地续写
    ST->>ST: 1. 记录 _partial_hit_reqs[req_id] =<br/>(block_idx, 共享尾块) [280-286]
    ST->>ST: num_cached_block[req_id] = block_idx
    Note over ST: 2. get_num_blocks_to_allocate 预留 +1 块 [223-226]
    ST->>BP: 3. get_new_blocks(1) → cow_block
    ST->>ST: 4. _apply_cow: [402-422]<br/>req_blocks[block_idx] = cow_block<br/>_pending_cow_copies += (source, cow)<br/>cow_block.ref_cnt += 1
    Note over ST,WK: 拷贝任务排队,块被保持引用不释放
    WK->>WK: 5. take_kv_cache_block_copies [840-855]<br/>GPU 上执行 source → cow 拷贝
    Note over ST: Mamba align 变体: 块表只追加,<br/>改走 move_block_hashes(source, cow)<br/>请求继续读 source, 拷贝填 cow [1631-1659]
```

---

## 图 5:KVCacheBlock 状态机(状态图)

```mermaid
stateDiagram-v2
    [*] --> FREE: 初始化(全部块入空闲链表)
    state FREE {
        [*] --> FRONT: 未缓存块(prepend_n → 队首 LIFO)
        [*] --> BACK: 缓存块(append_n → 队尾 FIFO/LRU)
    }
    FREE --> ALLOCATED: get_new_blocks<br/>popleft_n 摘下, ref_cnt 0→1 [647-677]
    ALLOCATED --> CACHED: cache_full_blocks / cache_partial_block<br/>_insert_block_hash 设哈希 [225-342]
    CACHED --> FREE: free_blocks<br/>ref_cnt 归 0 → append_n(队尾, LRU)[719-743]
    ALLOCATED --> FREE: free_blocks(无哈希)<br/>ref_cnt 归 0 → prepend_n(队首, LIFO)
    FREE --> ALLOCATED: touch(前缀命中重新锚定)<br/>ref_cnt==0 → remove 摘出 + ref_cnt+1 [702-717]
    CACHED --> ALLOCATED: touch(命中后继续使用)
    CACHED --> FREE: _maybe_evict_cached_block<br/>仅清哈希, 不动 ref_cnt/链表 [679-700]
    ALLOCATED --> ALLOCATED: 多请求共享(ref_cnt > 1)
    note right of CACHED
        驱逐两层语义:
        ① 哈希驱逐 = 清哈希元数据(ref_cnt>0 也可发生)
        ② 物理复用 = 从队首 popleft(LRU)
    end note
```

---

## 图 6:释放与延迟释放围栏(时序图)

```mermaid
sequenceDiagram
    autonumber
    participant SC as Scheduler
    participant KM as KVCacheManager
    participant BP as BlockPool

    Note over SC: defer_block_free 仅在<br/>max_concurrent_batches>1 且 KV consumer 时启用 [159-165]

    rect rgb(240, 250, 240)
        Note over SC: 立即释放路径(正常完成/无重叠批次)
        SC->>SC: _free_request_blocks [2463]
        SC->>SC: 判断: last_sched_seq <= processed_step_seq?<br/>(无在途 GPU 写)[2467-2471]
        SC->>KM: free(request) [2472]
        KM->>BP: free_blocks(reversed(blocks))
        Note over BP: 反序: 尾块先驱逐<br/>未缓存→prepend, 缓存→append
    end

    rect rgb(250, 240, 240)
        Note over SC: 延迟释放路径(重叠批次 + 在途写)
        SC->>KM: pop_blocks_for_free(request) [2474]
        KM-->>SC: KVCacheBlock 对象(块表已摘出)
        SC->>SC: deferred_frees.append(<br/>(sched_step_seq, blocks)) [2476]
        Note over SC: 围栏计数器:<br/>sched_step_seq += 1 → schedule 末尾 [1321]<br/>processed_step_seq += 1 → update_from_output 开头 [1761]
        SC->>SC: _drain_deferred_frees [2489]
        SC->>BP: 围栏满足 → free_blocks(reversed) [2502]
    end
```

---

## 图 7:抢占流程(流程图)

```mermaid
flowchart TD
    A[Phase 1: allocate_slots 返回 None<br/>scheduler.py:639] --> B{选择受害者<br/>:649-696}
    B -->|PRIORITY 策略| C[max 按 priority, arrival_time<br/>:652-655]
    B -->|FCFS 策略| D[running.pop 最新请求<br/>:686]
    C --> E[_preempt_request<br/>scheduler.py:1347]
    D --> E
    E --> F[1. _free_request_blocks<br/>:1363 释放 KV 块]
    F --> G[2. encoder_cache_manager.free<br/>:1364]
    G --> H[3. status = PREEMPTED<br/>num_computed_tokens = 0<br/>:1366-1367]
    H --> I[4. 清空 spec_token_ids<br/>num_preemptions += 1]
    I --> J[5. waiting.prepend_request<br/>:1387 回队头]
    J --> K{受害者是当前请求?<br/>:694-696}
    K -->|是| L[break: 本请求无法调度]
    K -->|否| M[重试 allocate_slots]
    M --> A
    J --> N[恢复时: CachedRequestData.resumed_req_ids<br/>块表替换而非追加]
```

---

## 图 8:reset_prefix_cache 流程(流程图)

```mermaid
flowchart TD
    A[reset_prefix_cache<br/>scheduler.py:2545] --> B{reset_running_requests?}
    B -->|是| C[循环: running.pop +<br/>_preempt_request<br/>drop_stale_output=True<br/>:2563-2565]
    C --> D[prev_step_scheduled_req_ids.clear<br/>:2571 强制 runner 刷新批次]
    B -->|否| E[要求无运行请求持块]
    D --> F[kv_cache_manager.reset_prefix_cache<br/>block_pool.py:764]
    E --> F
    F --> G{num_used_blocks == 1?<br/>只剩 null block}
    G -->|否| H[返回 False 或抛 RuntimeError<br/>:2574-2580]
    G -->|是| I[清空哈希表 + reset_hash 全部块<br/>:783-788]
    I --> J[返回 True]
```

---

## 图 9:推测解码拒绝回滚(流程图)

```mermaid
flowchart LR
    A[update_from_output<br/>scheduler.py:1844] --> B[num_rejected =<br/>num_draft - num_accepted]
    B --> C{output_is_stale?<br/>抢占后的陈旧计数}
    C -->|是| D[跳过回滚<br/>防止二次生效:1855]
    C -->|否| E[num_computed_tokens -= rejected<br/>:1857]
    E --> F[num_output_placeholders -= rejected<br/>异步调度下:1859]
    F --> G[KV 块不释放!<br/>下一步 Phase 1 重算 num_new_tokens<br/>重新推导并覆写被拒位置]
```

---

## 图 10:内存容量计算链(流程图)

```mermaid
flowchart TD
    A[GPUWorker.determine_available_memory<br/>gpu_worker.py:475] --> B{配置了 kv_cache_memory_bytes?}
    B -->|是| C[直接使用, 跳过 profiling]
    B -->|否| D[memory_profiling 上下文<br/>跑 profile_run<br/>skip_attn=True:515-519]
    D --> E[total_consumed = Δfree_memory<br/>mem_utils.py:317]
    E --> F[transient_peak_headroom =<br/>torch_peak - torch_allocated:323]
    F --> G[non_kv_cache_memory =<br/>total_consumed + headroom:326]
    G --> H[available_kv = requested_memory<br/>- non_kv_cache - cudagraph:559-563]
    H --> I[get_kv_cache_config_from_groups<br/>kv_cache_utils.py:1377]
    I --> J[num_blocks =<br/>avail // page_size // num_layers<br/>:1023-1040]
    J --> K[额外预留 1 个 null block<br/>:2243-2246]
    K --> L[KVCacheConfig.num_blocks<br/>→ BlockPool 构造<br/>kv_cache_coordinator.py:98]
```

---

## 附录:图中关键行号速查

| 调用 | 文件:行号 |
|---|---|
| allocate_slots 入口 | `kv_cache_manager.py:347` |
| Phase 1 调用点 | `scheduler.py:639` |
| Phase 2 调用点 | `scheduler.py:1041` |
| 前缀缓存查找 | `kv_cache_manager.py:232-298` |
| watermark 计算 | `kv_cache_manager.py:466-473` |
| 失败门 | `kv_cache_manager.py:524-530` |
| CoW `_apply_cow` | `single_type_kv_cache_manager.py:402-422` |
| free_blocks 双轨制 | `block_pool.py:719-743` |
| touch 重新锚定 | `block_pool.py:702-717` |
| 延迟围栏决策 | `scheduler.py:2463-2476` |
| 围栏排空 | `scheduler.py:2489-2502` |
| 抢占 | `scheduler.py:1347-1388` |
| 拒绝回滚 | `scheduler.py:1844-1865` |
| reset_prefix_cache | `scheduler.py:2545-2585` |
| num_blocks 公式 | `kv_cache_utils.py:1023-1040` |
| null block 创建 | `block_pool.py:190-191` |
