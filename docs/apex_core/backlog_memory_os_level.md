# 메모리 아키텍처 — OS 레벨 최적화 백로그

**작성일**: 2026-03-13
**상태**: 보류 (v0.6+ 운영 인프라 단계에서 재평가)
**관련 브레인스토밍**: v0.4.4.0 코드 리뷰 개선안 논의

---

## 보류 항목

### 1. NUMA 바인딩 + Core Affinity

**내용**: 코어 스레드를 물리 CPU에 고정(`pthread_setaffinity_np` / `SetThreadAffinityMask`)하고, 메모리 할당을 해당 NUMA 노드에 바인딩(`mbind`).

**기대 효과**: 멀티 소켓 환경에서 remote memory 접근 지연 제거 (로컬 ~70ns vs 원격 ~130ns, 1.5~2배 차이). CPU 캐시(L1/L2) warm 유지.

**현재 불필요 이유**: 싱글 소켓 환경에서는 NUMA 자체가 없음. per-core io_context 구조가 이미 cross-core 메모리 접근을 최소화.

**적용 시점**: 단일 프로세스가 2소켓 이상을 활용하는 배포 환경. v0.6+ 운영 인프라 단계.

**참고**: `numactl --cpunodebind=0 --membind=0`로 프로세스 레벨 바인딩만으로 80% 커버 가능.

---

### 2. mmap 직접 사용 (malloc 대체)

**내용**: 초기 대형 chunk 할당을 `malloc` 대신 `mmap`(Linux) / `VirtualAlloc`(Windows)으로 직접 수행.

**추가 제어권**:
- `madvise(MADV_DONTNEED)`: 유휴 페이지 OS 반환 → RSS 절감
- `mprotect`: guard page로 버퍼 오버런 감지
- `MAP_HUGETLB` / `MEM_LARGE_PAGES`: 대형 페이지 (TLB miss 감소)

**현재 불필요 이유**: malloc도 대형 할당 시 내부적으로 mmap/VirtualAlloc 호출. 기동 시 1회 할당이므로 런타임 성능 차이 0. API 경계만 깔끔하면 나중에 교체 가능.

**적용 시점**: v0.6+ RSS 모니터링, 메모리 프로파일링 도입 시.

---

### 3. Hugepage (대형 페이지)

**내용**: 4KB 대신 2MB 페이지로 메모리 풀 할당. TLB 엔트리 사용량 대폭 감소 (64MB chunk 기준 16,384개 → 32개).

**기대 효과**: 워크로드에 따라 5~30% 성능 개선 (랜덤 접근 패턴에서 효과 큼).

**현재 불필요 이유**: 벤치마크 기반 판단 필요. Linux THP(Transparent Huge Pages)로 일부 자동 적용될 수 있음.

**적용 시점**: v0.6+ 부하 테스트에서 TLB miss가 병목으로 확인된 경우.

---

### 4. Compaction / LSA (Log-Structured Allocator)

**내용**: Seastar LSA 방식의 메모리 조각 모음. bump-pointer 할당 + sparse 세그먼트 compaction.

**현재 불필요 이유**: bump+slab+arena 조합은 구조적으로 외부 단편화가 거의 발생하지 않음. Compaction이 필요한 건 가변 크기 할당이 빈번한 DB 캐시 워크로드이며, 네트워크 프레임워크 핫패스에는 해당 없음.

**적용 시점**: 해당 없음 (현재 아키텍처에서 필요성 낮음). GB급 in-memory 캐시 도입 시 재평가.

---

## 참고: 성능 영향 요약 (브레인스토밍 분석 결과)

| 항목 | 현재 설계 대비 손실 | 적용 시점 |
|------|-------------------|----------|
| mmap 직접 사용 | 0% (런타임) | v0.6+ |
| NUMA 바인딩 | 0% (싱글 소켓) ~ 20% (멀티 소켓) | v1.0+ |
| Compaction/LSA | ~0% (구조적 회피) | 해당 없음 |
| Hugepage | 5~15% (워크로드 의존) | v0.6+ |
| Core Affinity | 캐시 효율 개선 (정량화 필요) | NUMA와 동시 |

## 레퍼런스

- Seastar memory.cc: per-core chunk + LSA + NUMA mbind
- DragonflyDB: mimalloc mi_heap_t per-thread + shared-nothing
- Linux SLUB: per-CPU 객체 캐시 + NUMA 인식
