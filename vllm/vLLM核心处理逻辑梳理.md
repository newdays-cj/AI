# vLLM 核心处理逻辑梳理

> 基于 vLLM v1 引擎源码分析(仓库:`vllm-project/vllm`)
> 分析时间:2026-08-22

---

## 目录

1. [整体架构:一条请求的完整旅程](#一整体架构一条请求的完整旅程)
2. [引擎核心:EngineCore.step() 主循环](#二引擎核心enginecorestep-主循环)
3. [调度器:统一 Token 分配循环](#三调度器统一-token-分配循环)
4. [KV Cache 管理:PagedAttention 核心](#四kv-cache-管理pagedattention-核心)
5. [执行器与模型运行器:两阶段设计](#五执行器与模型运行器两阶段设计)
6. [注意力后端与采样](#六注意力后端与采样)
7. [完整链路一张图](#七完整链路一张图)
8. [核心设计哲学](#八核心设计哲学)

---

## 一、整体架构:一条请求的完整旅程

vLLM v1 采用**引擎核心(EngineCore)与前端分离**的设计。一次 `generate()` 调用经过以下层次:

```
用户代码
  │  LLM.generate(prompts, sampling_params)          vllm/entrypoints/llm.py
  ▼
OfflineInferenceMixin._run_engine()                   vllm/entrypoints/offline_utils.py
  │  循环: while has_unfinished_requests(): step()
  ▼
LLMEngine.add_request() / step()                      vllm/v1/engine/llm_engine.py
  │  负责: 输入预处理、输出还原,自身不做计算
  ▼
EngineCoreClient (InprocClient / SyncMPClient / AsyncMPClient)   vllm/v1/engine/core_client.py
  │  负责: 进程间传输(同进程直调,或 ZMQ + 后台进程)
  ▼
┌─────────────────────────────────────────────────────┐
│ EngineCore.step()  ← 核心内循环                      │
│                                                     │
│   Scheduler.schedule()      → SchedulerOutput       │
│   Executor.execute_model()  → ModelRunnerOutput     │
│   Scheduler.update_from_output() → EngineCoreOutputs│
└─────────────────────────────────────────────────────┘
```

### 1.1 入口层(Layer 0)

| 文件 | 类/方法 | 职责 |
|---|---|---|
| `vllm/entrypoints/llm.py` | `LLM.generate()` / `LLM.chat()` | 用户离线 API 入口,内部持有 `LLMEngine` |
| `vllm/entrypoints/offline_utils.py` | `OfflineInferenceMixin` | `_run_completion` → `_add_completion_requests` → `_run_engine`;`_run_engine` 中 `while has_unfinished_requests(): step()` 驱动整个推理 |

### 1.2 引擎前端(Layer 1)

**文件:** `vllm/v1/engine/llm_engine.py`

- `add_request()`:输入预处理(`InputProcessor.process_inputs` 分词) → 输出状态登记(`OutputProcessor.add_request`) → `engine_core.add_request()`
- `step()`:同步泵 — `engine_core.get_output()` → `output_processor.process_outputs()` → 中止已完成请求 → 返回 `RequestOutput`

异步版本 `vllm/v1/engine/async_llm.py` 的 `AsyncLLM` 用后台任务 `_run_output_handler()` 持续拉取输出。

### 1.3 传输层(Layer 2)

**文件:** `vllm/v1/engine/core_client.py`

- `InprocClient`:同进程直调 `engine_core.step_fn()`(离线 LLM 默认)
- `SyncMPClient` / `AsyncMPClient`:ZMQ push/pull 到后台 `EngineCoreProc` 进程

### 1.4 核心数据结构

**文件:** `vllm/v1/engine/__init__.py`

- `EngineCoreRequest`:`request_id`、`prompt_token_ids`、`sampling_params`、`priority` 等
- `EngineCoreOutput`:`new_token_ids`、`new_logprobs`、`finish_reason`
- `EngineCoreOutputs`:`outputs`、`scheduler_stats`、`timestamp`

---

## 二、引擎核心:EngineCore.step() 主循环

**文件:** `vllm/v1/engine/core.py`(line 583)

**这是整个系统的节拍器**,每一步执行:

```
1. scheduler.schedule()                    → SchedulerOutput(决定这一步跑谁、跑多少 token)
2. executor.execute_model(scheduler_output, non_block=True)   → 异步执行模型前向
3. scheduler.get_grammar_bitmask(...)      → 结构化输出约束(语法 bitmask)
4. future.result() 或 executor.sample_tokens()  → 采样出 token
5. scheduler.update_from_output(...)       → 把新 token 写回 Request,检测停止
6. 返回 EngineCoreOutputs
```

多进程模式下,`EngineCoreProc.run_busy_loop()`(line 1391)无限循环执行同样的步骤,通过 ZMQ/队列与前端通信。`_process_input_queue()` 处理 ADD/ABORT 请求,`_process_engine_step()` 执行一步推理并把结果放入输出队列。

**组件关系**:`EngineCore` 持有并协调 `Scheduler`(决策跑什么 → `SchedulerOutput`)和 `ModelExecutor`(执行模型 → `ModelRunnerOutput`);`Scheduler.update_from_output` 把模型输出还原为前端可消费的 `EngineCoreOutputs`。

---

## 三、调度器:统一 Token 分配循环

**文件:** `vllm/v1/core/sched/scheduler.py`

**核心心智模型**:没有独立的"prefill 阶段/decode 阶段"——只有一个 `num_computed_tokens` 进度计数器,每步都试图把它推向 `num_tokens_with_spec`。

### 3.1 数据结构

- `Request`(`vllm/v1/request.py`):
  - `num_computed_tokens`:已计算 token 数(唯一进度变量)
  - `num_tokens` / `num_tokens_with_spec`:总 token 数(含推测解码草稿 token)
  - `status`:`RequestStatus` 状态机
  - `block_hashes`:prefix cache 哈希链
  - `priority` / `arrival_time`:调度优先级依据
- 队列:
  - `waiting`:等待队列
  - `running`:运行中列表
  - `skipped_waiting`:被阻塞的等待队列(等待语法/远端 KV/流式输入)
- 调度策略 `SchedulingPolicy`:
  - `FCFS`:FIFO 双端队列
  - `PRIORITY`:小顶堆,`priority` 小者优先,同优先级按 `arrival_time`

### 3.2 Request 状态机

```
WAITING ──schedule()──▶ RUNNING ──check_stop()──▶ FINISHED_* (释放资源)
    ▲                      │
    │      _preempt_request()(释放 KV 块, num_computed_tokens=0)
    └────── PREEMPTED ◀────┘

阻塞状态: WAITING_FOR_STRUCTURED_OUTPUT_GRAMMAR / WAITING_FOR_REMOTE_KVS / WAITING_FOR_STREAMING_REQ
```

### 3.3 schedule() 两阶段

每步一个统一的 token 预算循环(`token_budget = max_num_scheduled_tokens`):

**Phase 1 — 补充 RUNNING 请求**:

```
num_new_tokens = num_tokens_with_spec - num_computed_tokens   # 剩余要算的 token
按 long_prefill_token_threshold、token_budget、max_model_len 截断
allocate_slots() 分配 KV 块 → 失败则抢占最低优先级请求
```

**Phase 2 — 接纳 WAITING 请求**:

```
1. prefix cache 查找: get_computed_blocks() → 命中块 + 命中 token 数
2. num_new_tokens = num_tokens - num_computed_tokens
   (chunked prefill: 被 request_token_budget 截断,即分块预填充)
3. allocate_slots() 分配新块 → 失败则 break
4. 出队 → 加入 running,状态 RUNNING
```

### 3.4 关键机制

| 机制 | 实现方式 |
|---|---|
| **Chunked prefill** | `num_new_tokens = min(剩余, token_budget)` — 长 prompt 分多步处理;若关闭 chunked prefill 且一步放不下则拒绝调度 |
| **抢占(Preemption)** | **纯重算(recompute-only)**:释放 KV 块,`num_computed_tokens=0`,状态 `PREEMPTED`,重新入队。**v1 没有 swap 到 CPU 的概念**(那是 v0 概念) |
| **优先级抢占选受害者** | PRIORITY:抢占 `max(priority, arrival_time)` 者;FCFS:抢占最新入队者 |
| **停止检测** | `check_stop()`(`vllm/v1/core/sched/utils.py`):EOS/stop token → `FINISHED_STOPPED`;长度 → `FINISHED_LENGTH_CAPPED`;重复检测 → `FINISHED_REPETITION` |

### 3.5 调度器输出

**文件:** `vllm/v1/core/sched/output.py`

- `SchedulerOutput`:每步执行计划 —
  - `scheduled_new_reqs: list[NewRequestData]`(首次请求,全量数据)
  - `scheduled_cached_reqs: CachedRequestData`(已知请求,增量 diff)
  - `num_scheduled_tokens: dict[str, int]`(每请求本步 token 数)
  - `finished_req_ids` / `preempted_req_ids`
- `NewRequestData`:`block_ids`(块表)、`prompt_token_ids`、`sampling_params`
- `CachedRequestData`:`new_block_ids`、`num_computed_tokens`、`resumed_req_ids`(被抢占恢复的请求,块表是**替换**而非追加)

---

## 四、KV Cache 管理:PagedAttention 核心

**文件:** `vllm/v1/core/kv_cache_manager.py` + `block_pool.py` + `kv_cache_utils.py`

### 4.1 物理结构

```
物理块池 BlockPool
  ├─ free_block_queue: 空闲块双向链表
  │    ├─ 非缓存块 LIFO 复用(GPU 局部性)
  │    └─ 缓存块 FIFO 淘汰(LRU)
  ├─ cached_block_hash_to_block: prefix cache 哈希表
  │    └─ 哈希冲突不驱逐,允许相同内容多副本(块表只追加、块 ID 永不变)
  └─ blocks[]: 每块 = {block_id, ref_cnt, block_hash}
```

- `KVCacheBlock`(`kv_cache_utils.py`):`block_id`、`ref_cnt`(引用计数)、`_block_hash`(满块才有)、空闲链表指针
- 每请求的块表:`SingleTypeKVCacheManager.req_to_blocks: dict[str, list[KVCacheBlock]]`

### 4.2 前缀缓存(Prefix Caching)

- **链式哈希**:`hash_block_tokens()` 计算 `hash(父块哈希, 本块token, extra_keys)` — 每块哈希天然指纹化其全部前缀
- 哈希粒度:`hash_block_size`(哈希用)vs `scheduler_block_size`(LCM)vs 每层 `block_size`,通过 `resolve_block_hashes` 协调
- extra_keys:LoRA 名 + 多模态特征 + `cache_salt` + prompt embeds
- 命中查找 `get_computed_blocks()`:`max_cache_hit_length = num_tokens - 1`(最后 1 个 token 必须重算以得到 logits),在哈希表中走最长前缀;`FullAttentionManager.find_longest_cache_hit` 先粗粒度整块查找,再细粒度块内探测

### 4.3 分配流程(allocate_slots 三步)

```
1. remove_skipped_blocks(): 清理滑动窗口外的块(全注意力无操作)
2. allocate_new_computed_blocks():
   ├─ add_local_computed_blocks(): touch() 命中块(引用计数+1, 移出空闲队列)
   └─ allocate_external_computed_blocks(): KV 连接器缓存的 token 分配新块
3. allocate_new_blocks(): get_new_blocks() 取新块
   └─ 部分命中时触发 Copy-on-Write(复制共享尾块为私有副本)
4. cache_blocks(): 新计算完的满块计算哈希、注册进哈希表
```

失败(块不够)→ 返回 `None` → 调度器执行抢占。

### 4.4 释放流程

`Scheduler._free_request_blocks()` → `kv_cache_manager.free()` → `block_pool.free_blocks()`,引用计数减到 0 后按 LIFO/FIFO 规则入空闲队列。异步批处理下支持延迟释放(deferred frees)以避免正在 GPU 上使用的块被复用。

### 4.5 Block 表流转

```
Scheduler 的 block_ids → SchedulerOutput.block_ids
  → GPUModelRunner 的 BlockTables(持久 GPU 张量 [num_reqs, max_blocks])
  → CommonAttentionMetadata.block_table_tensor
  → Attention 内核用 block_table 索引 KV cache 页
```

---

## 五、执行器与模型运行器:两阶段设计

### 5.1 RPC 链路

```
Executor.execute_model() → collective_rpc → 共享内存 MessageQueue
  → WorkerProc.worker_busy_loop → WorkerWrapperBase → Worker → GPUModelRunner
```

- `vllm/v1/executor/abstract.py`:`Executor` 是薄 RPC 门面,所有调用走 `collective_rpc`
- `vllm/v1/executor/multiproc_executor.py`:`MultiprocExecutor` + 每 GPU 一个 `WorkerProc`;流水线并行(PP)下,`output_rank`(最后一 PP 段、TP rank 0)负责回传结果;中间段用 `IntermediateTensors` 收发隐状态
- `vllm/v1/worker/gpu_worker.py`:`Worker.execute_model` 处理 PP 收发后调用 `model_runner.execute_model`;`init_device` 里做 KV cache 显存预算(`determine_available_memory` 跑一次 profile)

### 5.2 GPUModelRunner 两阶段(核心设计!)

**文件:** `vllm/v1/worker/gpu_model_runner.py`

**Phase A — `execute_model()`(返回 None,stash 状态)**:

```
1. _update_states():           更新 InputBatch(增删请求、追加 block table、condense 紧凑化)
2. _prepare_inputs():          构建 input_ids、positions、query_start_loc、seq_lens、slot_mapping
3. _build_attention_metadata():构建 CommonAttentionMetadata + 各层注意力元数据
4. _preprocess():              多模态编码 / embedding
5. _model_forward():           self.model(...) → hidden_states   ← 真正的模型前向
6. compute_logits():           hidden_states[logits_indices] → logits
7. 存入 ExecuteModelState,return None
```

**Phase B — `sample_tokens()`(紧接着被 EngineCore 调用)**:

```
1. 取出 stash 的 logits,应用语法 bitmask(结构化输出)
2. _sample(): Sampler(logits, sampling_metadata) → SamplerOutput
   普通解码 → Sampler;推测解码 → RejectionSampler
3. _bookkeeping_sync(): GPU token → CPU list
4. 返回 ModelRunnerOutput → 传回调度器进程
```

两阶段拆分的目的:模型前向(慢)与采样(快)解耦,支持异步调度与结构化输出约束注入。

### 5.3 InputBatch:扁平化批处理

**文件:** `vllm/v1/worker/gpu_input_batch.py`

把所有请求压平成固定形状的**持久张量**(跨步复用,避免每步重新分配):

- `token_ids_cpu_tensor [max_num_reqs, max_model_len]`
- `num_computed_tokens_cpu`、`block_table`
- 采样参数数组:`temperature` / `top_p` / `top_k` / 惩罚系数 / 随机数生成器
- `CachedRequestState`:每请求状态记录
- `condense()`:删除请求后紧凑化(persistent-batch 优化)
- `refresh_metadata()` → `_make_sampling_metadata()`:重建 `SamplingMetadata`

---

## 六、注意力后端与采样

### 6.1 后端选择与接口

**接口文件:** `vllm/v1/attention/backend.py`

- `AttentionBackend`(抽象):`get_builder_cls()` / `get_impl_cls()` / KV cache 形状 / 能力谓词(`supports_sliding_window` 等)/ `validate_configuration()`
- `AttentionMetadataBuilder.build(common_prefix_len, common_attn_metadata)` → 后端专属元数据
- `AttentionImpl.forward(layer, query, key, value, kv_cache, attn_metadata, output)`
- `CommonAttentionMetadata`(共享载体):`query_start_loc`、`seq_lens`、`block_table_tensor`、`slot_mapping`、`causal` 等

**选择链**:`vllm/v1/attention/selector.py::get_attn_backend()` → 平台(如 NVIDIA/ROCm/CPU)映射枚举到类路径 → 按层组为每层选后端。

**后端清单**(`vllm/v1/attention/backends/`):

| 类别 | 后端 |
|---|---|
| NVIDIA 主力 | `flash_attn.py`(FlashAttention v2/v3,默认)、`flashinfer.py`(native/XQA/trtllm-gen) |
| 通用 | `triton_attn.py`、`flex_attention.py`、`cpu_attn.py` |
| AMD | `rocm_attn.py`、`rocm_aiter_fa.py`、`rocm_aiter_unified_attn.py` |
| SSM/混合 | `mamba1_attn.py`、`mamba2_attn.py`、`short_conv_attn.py`、`linear_attn.py`、`gdn_attn.py` |
| MLA(DeepSeek 等)| `mla/flashmla.py`、`mla/flashinfer_mla.py`、`mla/triton_mla.py`、`mla/cutlass_mla.py` 等 |

### 6.2 Block Table 消费(以 FlashAttention 为例)

```
scheduler block_ids → BlockTable.commit_block_table() → block_table 张量 [num_reqs, max_blocks]
  → CommonAttentionMetadata.block_table_tensor
  → FlashAttentionMetadataBuilder.build(): 原样复制进 FlashAttentionMetadata
  → FlashAttentionImpl.forward(): block_table 索引 KV cache 页,slot_mapping 写入 K/V
```

**FlashInfer** 更复杂:`build()` 先 `split_decodes_and_prefills()` 拆分 batch,再 `_compute_flashinfer_kv_metadata()` 把密集 `block_table` 用 Triton 核转成 CSR 形式(`paged_kv_indptr` / `paged_kv_indices` / `paged_kv_last_page_len`);XQA/trtllm-gen 路径则直接在 GPU 上传 `block_tables`。

### 6.3 采样管线

**文件:** `vllm/v1/sample/sampler.py` — `Sampler.forward(logits, sampling_metadata)`

```
logits → float32 → allowed-token 掩码 → bad words 屏蔽
  → logit bias / min_tokens(可改变 argmax 的处理器)
  → 惩罚(presence / frequency / repetition)
  → 温度缩放 → min_p(argmax 不变的处理器)
  → top-k / top-p 过滤(TopKTopPSampler)
  → 采样(greedy 直接 argmax;随机用 Gumbel-max 避免 multinomial 同步)
  → SamplerOutput(sampled_token_ids, logprobs)
```

**SamplingMetadata 字段**(`vllm/v1/sample/metadata.py`):
`temperature` / `top_p` / `top_k` / `all_greedy` / `generators`(每请求 RNG,支持 seed 复现)/ `frequency_penalties` 等惩罚 / `output_token_ids` / `allowed_token_ids_mask` / `bad_words_token_ids` / `logitsprocs`。

**采样 ops**(`vllm/v1/sample/ops/`):

| 文件 | 职责 |
|---|---|
| `topk_topp_sampler.py` | `TopKTopPSampler` — top-k/top-p 过滤 + 加权采样,分发到 FlashInfer/XPU/CPU/原生实现 |
| `topk_topp_triton.py` | Triton 核加速 top-k/top-p 掩码 |
| `penalties.py` | `apply_all_penalties` — presence/frequency/repetition 惩罚 |
| `bad_words.py` | `apply_bad_words` — 屏蔽违禁词延续 token |
| `logprobs.py` | `batched_count_greater_than` — 采样 token 排名计算 |

**logits 处理器**(`vllm/v1/sample/logits_processor/`):`MinPLogitsProcessor`(argmax 不变)、`LogitBiasLogitsProcessor`、`MinTokensLogitsProcessor`,按 `argmax_invariant` / `non_argmax_invariant` 分组,在管线不同阶段应用。

---

## 七、完整链路一张图

```
EngineCore.step()
 │
 ├─ Scheduler.schedule()                 ← 决策: 跑谁、跑多少
 │    ├─ RUNNING 补token → allocate_slots
 │    ├─ WAITING 接纳 → prefix cache 命中 → allocate_slots
 │    └─ 输出 SchedulerOutput (block_ids, num_scheduled_tokens)
 │
 ├─ Executor.execute_model()             ← RPC → Worker → GPUModelRunner
 │    ├─ InputBatch 更新 (block table append)
 │    ├─ 构建 input_ids / positions / attention metadata
 │    ├─ self.model() 前向 → logits       ← 模型计算
 │    └─ stash logits, 返回 None
 │
 ├─ Executor.sample_tokens()             ← RPC → GPUModelRunner
 │    ├─ 语法 bitmask + Sampler (温度/top-k/top-p/惩罚)
 │    ├─ GPU→CPU 转换
 │    └─ 返回 ModelRunnerOutput (新 token ids)
 │
 └─ Scheduler.update_from_output()       ← 写回: num_computed_tokens+1, 检测停止
      └─ 完成 → 释放 KV 块 → EngineCoreOutputs → 前端 OutputProcessor
```

### 关键文件索引

| 模块 | 文件 | 核心类 |
|---|---|---|
| 入口 | `vllm/entrypoints/llm.py` | `LLM` |
| 引擎前端 | `vllm/v1/engine/llm_engine.py` | `LLMEngine` |
| 异步前端 | `vllm/v1/engine/async_llm.py` | `AsyncLLM` |
| 传输 | `vllm/v1/engine/core_client.py` | `InprocClient` / `SyncMPClient` / `AsyncMPClient` |
| 引擎核心 | `vllm/v1/engine/core.py` | `EngineCore` / `EngineCoreProc` |
| 调度器 | `vllm/v1/core/sched/scheduler.py` | `Scheduler` |
| 请求 | `vllm/v1/request.py` | `Request` / `RequestStatus` |
| KV 管理 | `vllm/v1/core/kv_cache_manager.py` | `KVCacheManager` |
| 块池 | `vllm/v1/core/block_pool.py` | `BlockPool` |
| 块工具 | `vllm/v1/core/kv_cache_utils.py` | `KVCacheBlock` / `FreeKVCacheBlockQueue` / `hash_block_tokens` |
| 执行器 | `vllm/v1/executor/abstract.py` | `Executor` |
| 多进程执行 | `vllm/v1/executor/multiproc_executor.py` | `MultiprocExecutor` / `WorkerProc` |
| Worker | `vllm/v1/worker/gpu_worker.py` | `Worker` |
| 模型运行器 | `vllm/v1/worker/gpu_model_runner.py` | `GPUModelRunner` |
| 输入批处理 | `vllm/v1/worker/gpu_input_batch.py` | `InputBatch` / `CachedRequestState` |
| 注意力接口 | `vllm/v1/attention/backend.py` | `AttentionBackend` / `CommonAttentionMetadata` |
| 采样器 | `vllm/v1/sample/sampler.py` | `Sampler` |
| 采样元数据 | `vllm/v1/sample/metadata.py` | `SamplingMetadata` |
| 输出结构 | `vllm/v1/outputs.py` | `ModelRunnerOutput` / `SamplerOutput` |

---

## 八、核心设计哲学

1. **统一 Token 预算循环**:调度器只维护 `num_computed_tokens` 一个进度变量,把"选谁跑、跑多少、KV 块怎么分"统一为每步一次的 token 预算分配;prefill/decode 不是两个阶段,而是同一循环的两类请求。

2. **纯重算抢占**:v1 放弃 v0 的 swap 机制,抢占即释放 KV 块 + 清空进度计数器 + 重新入队,简单且避免 CPU-GPU 传输。

3. **持久扁平化批处理**:InputBatch 用固定形状的持久张量表示整个 batch,配合 condense 紧凑化,免去每步的分配开销。

4. **两阶段执行**:`execute_model`(前向 + logits,返回 None + stash 状态)与 `sample_tokens`(采样 + bookkeeping)分离,支持异步调度与结构化输出 bitmask 注入。

5. **链式哈希前缀缓存**:块哈希包含父块哈希,天然表达前缀关系;块表只追加、块 ID 永不变,哈希冲突靠多副本容忍,换来 O(1) 的命中查找与零碎块管理。

6. **引擎核心与前端分离**:EngineCore(决策 + 计算)与 LLMEngine/AsyncLLM(协议 + 输入输出处理)解耦,同进程直调或跨进程 ZMQ 皆可,离线与在线服务共享同一内核。
