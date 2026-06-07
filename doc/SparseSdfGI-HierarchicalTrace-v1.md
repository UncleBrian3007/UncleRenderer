# SparseSdfGI Hierarchical Trace v1

## 목표

SparseSdfGI trace를 기존 flat sphere march + brick 단위 skip에서, FFX Brixelizer와 같은 고정 stamp 계층 DDA로 전환한다.

v1은 multi-cascade가 아니라 현재 단일 SparseSdfGI cascade의 `64^3` cascade cell grid 내부 acceleration이다.

```text
16 cascade cells -> 4 cascade cells -> 1 cascade cell -> brick-local AABB -> inner SDF march <= 8
```

## Root Signature Phase 0

기존 SparseSdfGI root signature 비용은 다음과 같았다.

```text
b0 CBV 2 DWORD + b1 root constants 50 DWORD + b2 bindless constants 11 DWORD = 63 DWORD
```

hierarchy SRV 2개를 trace/probe bindless layout에 추가하면 65 DWORD가 되어 D3D12 root signature 64 DWORD 제한을 넘는다.

따라서 v1은 먼저 `SparseSdfGIConstants : b1`을 root constants에서 root CBV로 전환한다. C++는 per-frame mapped constant buffer에 `FSparseSdfGIConstants`를 256-byte aligned slot으로 업로드하고, 각 dispatch에서 `SetComputeRootConstantBufferView(1, address)`로 바인딩한다.

## Hierarchy Layout

- bottom hierarchy: `16^3` nodes, 각 node는 `4^3` cascade cells를 대표한다.
- top hierarchy: `4^3` nodes, 각 node는 `4^3` bottom nodes, 즉 `16^3` cascade cells를 대표한다.
- node format: `uint4(minPacked, maxPacked, flags, reserved)`.
- coord packing: cascade cell coord `0..63`을 축당 6bit로 packing한다.
- flag: `SPARSE_SDF_GI_TRACE_HIERARCHY_OCCUPIED`.

bottom build pass는 `CascadeBrickMap + BrickMetadata`를 읽어 occupied brick coord bounds를 만든다. top build pass는 bottom nodes를 reduce한다. leaf에서는 기존 `BrickMetadata`의 brick-local AABB를 다시 검사하므로 hierarchy node AABB는 보수적이어도 된다.

## Pass 순서

SDF cache rebuild가 발생한 프레임에만 hierarchy를 다시 만든다.

```text
Reference Init
Emit Triangle References
Solve Brick References
Build Trace Hierarchy Bottom
Build Trace Hierarchy Top
Reference Stats Present
```

SDF cache가 유효한 프레임에는 기존처럼 stats present만 수행하고 hierarchy는 persistent resource를 그대로 사용한다.

## Trace 경로

`SparseSdfGIUseHierarchicalTrace=true`일 때 `TraceSdfVisibility`와 `TraceSdfDebugSurface`는 hierarchy traversal을 사용한다.

- cascade AABB에 ray를 clip하고 `max(tEnter, 0)`에서 시작한다.
- top cell, bottom cell, leaf cell 순서로 현재 travel의 DDA cell을 찾는다.
- 빈 top/bottom node는 해당 cell exit까지 한 번에 skip한다.
- leaf cell은 `BrickMetadata` AABB와 ray가 겹칠 때만 inner SDF march를 최대 8회 수행한다.
- debug hit payload는 DDA leaf cascade cell coord를 hit brick coord로 고정하고 local/frac만 hit position에서 계산한다.

`SparseSdfGIUseHierarchicalTrace=false`일 때는 bring-up A/B 비교를 위해 기존 flat path를 유지한다.

## 검증

- Debug Mode: Ray Trace, Step Count, Brick ID, Hit UVW, Brick Local Gradient.
- Probe path: screen probe trace가 per-pixel trace와 같은 hierarchy path를 쓰는지 확인한다.
- 주요 케이스: 빈 cascade 장거리 ray, occupied cell 밀집 영역, cascade 밖/안 시작 ray, 축평행에 가까운 ray, Exact Shared Border SDF.
- 합격 기준: root signature 생성 실패 없음, invalid bindless index log 없음, device removed 없음, Brick ID/UVW boundary jitter 감소.
