# Contract: `get_process_cpu(pid)` — for Agent 1/3

Status: **proposed**. QA does not ship the builtin. This document is the
contract that Phase 2 anomaly detection depends on. If the shipped
builtin differs from this in any observable way, Phase 2's baseline and
anomaly tests must be updated *and* the divergence documented.

## Signature

```donky
get_process_cpu(pid) -> number