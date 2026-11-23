# ARM64 Conditional Device TLB Notifier: Evidence and Continuation Plan

## Review Scope

Review this executable documentation/continuation plan and the linked phase
artifacts for clarity, verifiability, completeness, and consistency. In particular,
validate the technical return from architecture analysis to requirements
(workflow section ③→①), not a conclusion that the requested feature is impossible.
The user has preapproved future human gates; missing technical facts are not
automatically approved. No implementation, commit, or hardware verification is
claimed. This is not a request to approve speculative kernel changes.

## Confirmed Requirement

ARM64 user-address-space invalidations only. The issuing CPU waits for TLBI
completion, then reads its own 32-bit read-clear MMIO register. Bit 0 set invokes
a NEW MMU notifier callback associated with the target mm and address range.
The driver must synchronously finish device TLB invalidation before returning.
Existing notifier behavior must be preserved. New DT discovery and mapping are
in scope; independent per-CPU MMIO regions are provisional. Physical addresses
are TBD and can be supplied later by DT.

## Evidence Files

All paths below are absolute; read the artifacts as a dossier, not as implemented
design. A final editorial pass may add the completed Oracle findings.

- Requirements: /home/chenjun/workspace/arm64-tlbi-device-notifier/01-需求描述.md
- Research: /home/chenjun/workspace/arm64-tlbi-device-notifier/02-现有方案与上游调研.md
- Architecture: /home/chenjun/workspace/arm64-tlbi-device-notifier/03-架构分析.md
- Conditional alternatives: /home/chenjun/workspace/arm64-tlbi-device-notifier/04-方案对比.md
- Workflow: /home/chenjun/my-env/AI/linux_kernel_dev/需求开发.md
- Kernel root: /home/chenjun/Git/OLK-6.6

Key source evidence relative to the kernel root:

- arch/arm64/include/asm/tlbflush.h:253: existing full-mm callback follows DSB.
- arch/arm64/include/asm/tlbflush.h:265: page nosync callback precedes final DSB.
- arch/arm64/include/asm/tlbflush.h:418: range nosync and full-mm fallback.
- arch/arm64/include/asm/tlbflush.h:327: batch completion has no mm/range.
- arch/arm64/include/asm/tlbbatch.h:5: architecture batch structure is empty.
- mm/rmap.c:648: reclaim task batch accepts target mm from each mapping.
- arch/arm64/include/asm/pgtable.h:974: young clearing intentionally elides DSB.
- arch/arm64/mm/contpte.c:511: contiguous-PTE young clearing also elides DSB.
- include/linux/mmu_notifier.h:190: existing callback cannot sleep under PTL.
- mm/mmu_notifier.c:588: synchronous SRCU-protected notifier dispatch.

## Completed Feasibility Review

Oracle session ses_eb66e2c17ffeRIIArLrXY4nbjV concluded: stop before
implementation. Neither invent a vendor-neutral DT hardware ABI nor ship an
unused notifier/provider/consumer scaffold. This is a hardware-contract blocker,
not evidence that current CPU TLB handling is broken.

Candidate A conditionally completes active user nosync operations inline and
disables reclaim deferral, avoiding lost multi-mm metadata and missed young-bit
paths. CPU pinning and maskable IRQ exclusion must cover issue/DSB/status-read;
save status and leave that exclusion before traversing callbacks, subject to the
caller's original IRQ state. Old callbacks remain independent of bit 0. Their
active-path pre-DSB timing may need strengthening to post-DSB: this is an explicit
compatibility question, not permission to silently change the requirement.
Candidate B retains delayed completion but needs multiple mm/range/issuing-CPU
records with correct lifetimes, bounded memory use, and CPU-local completion.
Neither candidate is an approved, proven hardware implementation.

## Minimum Inputs Required Before Implementation

1. Hardware event attribution and sticky aggregation across KPTI, range loops,
   workaround repeats, broadcast, global/reserved-ASID and nested producers.
2. Status visibility after the issuing CPU's completion DSB, or a documented
   synchronization protocol. Arbitrary extra barriers do not invent this guarantee.
3. Register offset, endianness, reset state, read-clear atomicity/granularity and
   ownership of other bits; exclude or explain NMI/higher-EL interference.
4. A real platform DT hardware ABI associating CPUs and MMIO resources, plus
   initialization/quiescence, partial-failure, power and hotplug rules.
5. The actual consumer's non-sleeping, synchronous device invalidation and failure
   protocol. Status bit 0 requests a flush; reading it does not prove device completion.

## Executable Next Steps

1. Correct factual errors in the dossier and archive the completed Oracle verdict
   and blocker checklist in 00-进度与阻塞.md. No kernel changes.
2. Obtain Momus review of phase-1/phase-2 evidence and the ③→① technical-return
   condition using this valid plan path. Record the verdict accurately. An earlier
   direct requirement-document review was input-rejected because the reviewer
   requires a .omo/plans/*.md path; it was not a technical approval or rejection.
3. Validate all dossier references and the generated knowledge-base JSON; inspect
   git status to verify the kernel remains unchanged. Report no build/KUnit/QEMU/
   coverage results, since none have been run for an implementation.
4. Mark implementation [blocked], retain the user's automatic human-gate approval,
   and provide the minimal required inputs without demanding an immediate reply.
## Future Work, Not Authorized by This Execution Plan

Steps 5-6 below require authoritative inputs and a separately reviewed execution
plan with implementation-specific commands, tests, and pass/fail criteria. They
are continuation context, not current executable tasks or acceptance obligations.

5. When authoritative inputs arrive, revise requirements and phase-4 comparison,
   then complete detailed design and test design before coding. Cover normal,
   fallback, contiguous-PTE, young-bit, reclaim, multi-mm, migration, nesting,
   initialization/hotplug, config-off and subscriber-lifetime cases. Preserve
   notifier semantics or explicitly resolve the timing compatibility question.
6. Only after a decision-complete reviewed design: implement without placeholders,
   compile against ~/tools/qemu_arm64_defconfig, run checkpatch, execute software
   tests/QEMU and separately qualify the real MMIO/device protocol. Do not commit
   without explicit authorization. Mocks must not be reported as hardware proof.

## Per-Task QA for Current Steps 1-4

Run shell commands from `/home/chenjun/Git/OLK-6.6`. The dossier root is
`/home/chenjun/workspace/arm64-tlbi-device-notifier`. Markdown review uses the
Read tool against the complete relevant files; this is documentation QA, not
validation of the hardware or an unimplemented kernel feature.

### QA-1: Dossier Corrections (Step 1)

Tools: Read, Bash/rg. Read 00-04. Use
`rg -n 'STOP BEFORE IMPLEMENTATION|③→①|bit0|MMIO|young|回调|IRQ' /home/chenjun/workspace/arm64-tlbi-device-notifier/*.md`
to locate claims, then inspect their context. Compare 04 section 9 with
`mm/rmap.c:1781-1796,2150-2170` using Read; compare deferred young paths with
`arch/arm64/include/asm/pgtable.h:974-991` and `arch/arm64/mm/contpte.c:511-530`.
Expected: defer=false skips batch-add; young paths stay in scope; IRQ exclusion
starts before issuing TLBI; status is a request gate, not device completion;
pre/post-DSB old-callback timing remains an explicit unresolved boundary. No
unqualified claim of preserved exact active-path timing or hardware correctness.

### QA-2: Review Verdict Transcription (Steps 1-2)

Tools: background_output for actual completed reviews, Read for 00 and 04.
Compare the Oracle result from `bg_de387bf2` to the archived session ID and
STOP verdict. Compare each actual Momus result from `bg_9ef28518` to 00 section 5.
Expected: input-path rejection and later missing-QA rejection are recorded as
distinct events; no approval is claimed until a later actual approval arrives.
After the revised plan is reviewed, update 00 with the exact new verdict and
its scope. An approved blocked handoff is not approval of the kernel feature.

### QA-3: JSON and Source-Tree Integrity (Step 3)

Tool: Bash. Run
`python3 -m json.tool /home/chenjun/workspace/arm64-tlbi-device-notifier/kb/OLK-6.6/arch/arm64/mm/overview.json > /dev/null`.
Expected: exit 0; this checks JSON syntax only. Run `GIT_MASTER=1 git status --short`,
`GIT_MASTER=1 git diff --exit-code HEAD --` and
`GIT_MASTER=1 git diff --cached --exit-code`.
Expected: both diffs exit 0, status equals the recorded pre-work baseline
`?? AGENTS.md`. All new dossier files are outside the repository. Any deviation
must be investigated, not reset or removed. Read every changed dossier/plan file;
no code diagnostics/build/test claim is made for these Markdown/JSON artifacts.

### QA-4: Blocked Handoff and Resume Criteria (Step 4)

Tools: Read, Bash/rg. Run
`rg -n 'blocked|阻塞|未开始|未执行|自动|契约' /home/chenjun/workspace/arm64-tlbi-device-notifier/00-进度与阻塞.md`.
Expected: 00 explicitly states `[blocked]`, no implementation, no kernel build/
KUnit/QEMU/gcov run, preapproved human gates, and the missing hardware/consumer
checklist. Read steps 5-6 above and 00: both must require a separately reviewed
design/execution plan when the missing facts arrive. Expected: no immediate
response demanded from the absent user, no fictitious compatible or consumer,
and no implication that unavailable hardware testing has passed.

## Acceptance for This Handoff

- Confirmed user scope is distinct from proposals and missing hardware facts.
- Multi-mm and deferred young-bit paths are not silently excluded.
- No fabricated compatible, address, status visibility or device completion.
- Review rejection/approval and implementation/test status are reported honestly.
- Dossier and continuation criteria are sufficient for later work to resume without
  repeating the requirements interview; production implementation remains blocked.
