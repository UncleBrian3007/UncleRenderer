# SparseSdfGI v3

SparseSdfGI V3는 조밀 프로토타입을 Brixelizer 형태의 빌드 파이프라인으로 옮기기 시작하는 단계입니다. V3.1은 삼각형별 복셀 스캐터를 분할 참조 바이닝으로 대체하고, V3.2는 그 참조 데이터를 사용해 재빌드 프레임에서 점유 브릭만 풉니다.

## V3 백본

V3는 V2 조밀 프로토타입을 Brixelizer 형태의 희소 빌드 파이프라인으로 전환하는 세대입니다. 초점은 빌드 경로이며, 트레이싱/캐시(V2.4-V2.6)와 전체 GI 품질은 별도 관심사입니다.

```text
완료
  V3.1  분할 참조 바이닝(삼각형 풀 + 브릭 참조) - MaxTriangleVoxelSpan 홀 제거
  V3.2  점유 브릭 간접 solve 디스패치 - 재빌드 프레임에서 빈 브릭 no-op 스킵

계획(빌드 파이프라인, Brixelizer 동등성 방향)
  V3.x  압축 참조 레이아웃(scan/compact 연속 목록, 연결 리스트 대체)
        - solve 지역성 향상, 참조별 head InterlockedExchange 제거
  V3.x  삼각형 풀 압축(full f32 anchor + f16 edges, 약 24 B/triangle)
  V3.x  그룹 로컬 emit 예약(요소당이 아닌 그룹당 글로벌 atomic 1회)
        - 우선순위 하향: emit은 현재 1ms 미만이며 조밀/동적 씬에서만 의미 큼
  V3.x  증분 dirty-brick 디스패치(프레임 간 변경 브릭만 재-solve)
        - Brixelizer dirty 모델, 지속 브릭 상태 + 변경 추적 필요, 동적 지오메트리 지원 기반
  V3.x  희소 브릭 할당 + free list(조밀 brickId == cell 대신 실제 브릭 풀)
  V3.x  스크롤/랩을 갖는 다중 캐스케이드 클립맵
  V3.x  정적/동적 인스턴스 처리 및 브릭/캐스케이드 병합
```

### 상태와 우선순위

V3.2 시점에서 빌드는 정적 씬 기준 빠르고 완전합니다. 홀이 없고, 정상 상태 트레이스는 약 0.8ms, 재빌드는 약 5-8ms이며 캐시됩니다(강제 재빌드 또는 씬/설정 변경 시에만). 따라서 남은 빌드 파이프라인 항목은 정상 상태 비용보다 다음 두 목표에 의해 결정됩니다.

- 스케일과 동적성: 증분 dirty 디스패치, 희소 할당, 다중 캐스케이드는 큰 씬이나 애니메이션 씬에 필요합니다. 정적 단일 캐스케이드 프로토타입에는 이점이 적습니다.
- 메모리: 압축 참조와 삼각형 압축은 일시적 빌드 메모리 풋프린트를 줄입니다.

사용자 관점의 가장 큰 공백은 빌드 파이프라인이 아니라 GI 품질입니다. 확산 트레이스가 여전히 실제 반사 복사휘도가 아닌 오클루전/AO 플레이스홀더이기 때문입니다. 이 작업(히트 복사휘도, 복사휘도 캐시)은 V3 빌드 백본과 별도로 추적되며 독립적으로 진행할 수 있습니다.

## Radiance (라이팅 트랙)

확산 트레이스의 라이팅/복사휘도 작업(Radiance v1 및 향후 로드맵)은 빌드 백본과 분리되어 [SparseSdfGI-Radiance-v1.md](SparseSdfGI-Radiance-v1.md)에서 다룹니다. V3 빌드 작업과 독립적으로 진행할 수 있습니다.

## V3.1 분할 참조 바이닝

V3.1 빌더는 `MaxTriangleVoxelSpan` 드롭 경로를 제거합니다. 큰 삼각형은 더 이상 스킵되지 않으며, 대신 각 정적 메시 삼각형을 일시적 삼각형 풀에 한 번 저장하고, 경량 브릭 참조가 해당 삼각형을 가리킵니다.

```text
CSInitReferenceBuild
  -> SDF 아틀라스, 브릭 메타데이터, 조밀 브릭 맵, 참조 헤드, 카운터 클리어
  -> 정적 일반 메시별 CSEmitTriangleReferences
  -> CSSolveBrickReferences
  -> trace/debug는 SdfAtlas + CascadeBrickMap + BrickMetadata 읽음
```

emit 패스는 브릭 좌표 변환 전에 삼각형 경계를 `SurfaceThicknessVoxels * VoxelSize`만큼 확장합니다. 이는 브릭 경계 근처 삼각형이 인접 브릭에도 시드하도록 하기 위해 필요합니다. solve 패스는 브릭별 참조를 수집해 복셀별 협대역 시드를 계산하고, 기존 브릭 로컬 Eikonal 완화를 수행한 뒤, 트레이스와 같은 표면 임계값으로 브릭 메타데이터를 기록합니다.

## V3.2 점유 브릭 간접 Solve

V3.2는 재빌드 프레임의 조밀 `64^3` solve 디스패치를 제거합니다. 이제 참조 emit은 유효 참조가 특정 브릭에 처음 닿을 때만 그 브릭을 `OccupiedBrickList`에 추가합니다. 첫 터치 판정은 연결 리스트 head push를 재사용합니다. 즉 `InterlockedExchange(referenceHeads[brick], referenceId, oldHead)`가 `INVALID`를 반환하면 해당 브릭은 방금 첫 유효 참조를 받았다는 뜻입니다.

```text
CSInitReferenceBuild
  -> SDF 아틀라스, 브릭 메타데이터, 조밀 브릭 맵, 참조 헤드, 카운터 클리어
  -> 정적 일반 메시별 CSEmitTriangleReferences
  -> OccupiedBrickList에 대해 직접 청크 디스패치 CSSolveBrickReferences
  -> trace/debug는 SdfAtlas + CascadeBrickMap + BrickMetadata 읽음
```

이것은 처음부터 다시 빌드할 때의 최적화입니다. 빈 브릭은 init 패스가 기록한 `SdfAtlas = 1.0`, `BrickMetadata = 0` 값을 유지하고, 점유 브릭만 V3.1과 동일한 gather, Eikonal 완화, 메타데이터 축소를 수행합니다. 또한 Brixelizer 스타일 dirty-brick 디스패치로 가는 첫 단계이지만, 진짜 증분 dirty 추적은 향후 작업입니다.

### Solve 디스패치와 LDS 참조 캐싱

solve는 별도의 indirect-args 준비 패스(`CSPrepareSolveBrickReferencesArgs`)와 `ExecuteIndirect`를 사용하지 않고 **직접 청크 디스패치**합니다. CPU는 보수적으로 전체 브릭 수만큼 그룹을 `kSparseSdfGISolveDispatchChunkGroups` 청크 단위로 디스패치하며 청크마다 `BuildWorkOffset`를 전달합니다. 셰이더는 `ReferenceCounters`에서 점유 브릭 수를 읽어 `BuildWorkOffset + groupId.x >= occupiedBrickCount`이면 즉시 반환하므로, 실제 작업은 `OccupiedBrickList`의 점유 브릭에만 발생합니다. 이로써 GPU에서 인자를 채우는 추가 패스와 indirect 디스패치 의존을 제거했습니다.

solve 패스의 GPU 행은 LDS 참조 캐싱으로 해결했습니다. 이전에는 512개 복셀 스레드가 각자 브릭의 참조 연결 리스트를 walk하며 삼각형을 전역 메모리에서 반복 fetch했고, 긴 참조 리스트에서 행이 발생했습니다. 현재는 lane 0이 리스트를 한 번 walk해 삼각형 위치(`P0/P1/P2`)를 LDS 캐시(`SPARSE_SDF_GI_SOLVE_TRIANGLE_CACHE = 256`)에 적재하고, 512개 복셀 스레드가 그 배치를 공유해 점-삼각형 거리를 계산합니다. 리스트가 캐시 용량을 넘으면 lane 0이 다음 배치를 다시 채우는 식으로 반복해 LDS 예산 안에서 모든 참조를 처리합니다.

reference-build 경로는 vertex/index fetch, draw range, 캐스케이드 밖 삼각형, 브릭 span, solve 카운터, 할당, bindless 바인딩에 대한 경계 검사로 강화되었습니다. 진단용으로 `SparseSdfGIDebugSolveGroupBudget` / `SparseSdfGIDebugEmitTriangleBudget`(기본 무제한)로 solve 그룹과 emit 삼각형 작업량을 제한하거나 스킵해 행/디바이스 제거 원인을 분리할 수 있습니다.

## 리소스

- `TrianglePool`: emit된 삼각형마다 비압축 월드 공간 삼각형 엔트리 1개.
- `BrickReferenceHeads`: 조밀 브릭마다 연결 리스트 head 1개.
- `BrickReferences`: triangle id와 next 포인터를 담는 경량 노드.
- `ReferenceCounters`: triangle 수, reference 수, triangle 오버플로, reference 오버플로, occupied brick 수.
- `OccupiedBrickList`: 현재 재빌드에서 최소 1개 유효 참조를 가진 일시적 조밀 브릭 인덱스 목록. solve는 이 목록을 직접 청크 디스패치로 순회하며, 점유 브릭 수는 `ReferenceCounters`에서 읽어 셰이더에서 경계 검사한다.

V3.1/V3.2는 단순성을 위해 연결 리스트를 사용합니다. 압축 참조 레이아웃, 삼각형 압축, 진짜 증분 dirty-brick 디스패치는 이후 V3 작업으로 미룹니다.

## 제약 사항

- 조밀 단일 캐스케이드만 지원.
- 삼각형 풀은 비압축이며, Brixelizer 스타일 삼각형 압축 저장은 향후 작업.
- reference 오버플로는 정확도 손실이며 예산 문제로 다뤄야 함.
- 정적 SDF 캐시가 여전히 정상 상태 비용을 지배하며, V3.2는 캐시 히트 트레이스 비용이 아니라 재빌드 히치를 주로 줄임.

## 참고 / 출처

- AMD FidelityFX Brixelizer / Brixelizer GI 구조에서 영감을 받음.
- FidelityFX SDK 소스, 셰이더 코드, 데이터 테이블은 복사하지 않음.
