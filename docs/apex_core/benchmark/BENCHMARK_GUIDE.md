# 벤치마크 실측 노하우

## 실행 전 체크리스트

- [ ] 다른 프로그램 종료 (브라우저, IDE 등 — CPU 경합 방지)
- [ ] 전원 설정: "고성능" 모드 (노트북은 배터리 절약 모드에서 터보 부스트가 제한됨)
- [ ] Release 빌드 확인 (`apex-agent queue build release`)
- [ ] 벤치마크 실행 파일이 최신인지 확인 (`bin/release/bench_*.exe` 타임스탬프)

## 알려진 함정

### tick_interval 설정
`bench_cross_core_latency`는 CoreEngine의 `tick_interval`에 민감하다.
100ms로 설정하면 10,000회 ping-pong에 최대 33분이 소요될 수 있다.
**1ms로 설정**하여 드레인 주기를 줄여야 한다. 현재 코드에 반영 완료.

### cross-core latency의 spin-wait
외부 스레드에서 spin-wait하면 CoreEngine 드레인 타이밍과 맞지 않아 타임아웃된다.
**`std::binary_semaphore`로 동기화**하여 해결. 현재 코드에 반영 완료.

### Google Benchmark cpu_time vs real_time
멀티스레드 벤치마크에서 `cpu_time`은 **벤치마크 메인 스레드만** 측정한다.
워커 스레드의 실행 시간은 포함되지 않으므로, 반드시 `->UseRealTime()`을 사용하고
`real_time`과 `items_per_second`를 기준으로 분석해야 한다.
메인 스레드도 워커로 참여시키면 cpu_time이 합리적인 값으로 채워진다.

### 벤치마크 워커 수 설정
물리 코어 수에 맞춰 워커를 배치한다. HyperThreading 영역(물리 코어 초과)에서는
확장률이 완만해지므로, **물리 코어 범위가 선형 확장의 최적 구간**이다.
현재 i5-9300H(4물리/8논리)에서는 1~4워커가 최적.

## 아키텍처 비교 벤치마크 설계 원칙

### 공정한 비교
- Shared 모델에는 **업계 표준 최적화**(64-shard mutex)를 적용한다
- 단순 `++counter` 대신 **현실적 핸들러 워크로드**(세션 조회 + 상태 수정)를 사용한다
- Per-core도 Shared도 동일한 워크로드를 처리해야 한다

### Per-core가 빛나는 워크로드
Per-core의 이점은 **stateful 메시지 처리**에서 극대화된다:
- 매 메시지마다 세션 맵 조회 + 상태 읽기/수정
- Per-core: 자기 세션 맵에 직접 접근 (lock 제로)
- Shared: 모든 스레드가 공유 세션 맵에 mutex로 접근
- 추가로, 단일 io_context의 내부 핸들러 큐 경합이 근본 병목

### Shared 모델의 진짜 병목
세션 mutex를 아무리 최적화해도 (샤딩, strand, lock-free 등),
**단일 io_context의 내부 핸들러 큐**가 근본적 병목으로 남는다.
io_context::run()을 여러 스레드가 호출하면 내부 동기화가 발생한다.
io_context 분리만이 이 병목을 해결한다.

## 보고서 생성

### 신규 버전 벤치마크 워크플로우
```bash
VERSION="v0.6.0.0"
BASELINE="v0.5.10.0"

# 1. Release 빌드
apex-agent queue build release

# 2. 전체 벤치마크 실행
for bench in mpsc_queue spsc_queue allocators ring_buffer frame_codec \
             dispatcher timing_wheel session_lifecycle serialization \
             cross_core_latency cross_core_message_passing \
             frame_pipeline session_throughput architecture_comparison; do
    apex_core/bin/release/bench_${bench}.exe \
        --benchmark_format=json \
        --benchmark_out=apex_core/benchmark_results/${bench}.json
done

# 3. 버전 디렉토리로 복사
mkdir -p docs/apex_core/benchmark/${VERSION}
cp apex_core/benchmark_results/*.json docs/apex_core/benchmark/${VERSION}/
# metadata.json 생성 (에이전트가 자동 처리)

# 4. 보고서 생성 (버전 비교)
python apex_tools/benchmark/report/generate_benchmark_report.py \
    --data-dir=docs/apex_core/benchmark \
    --baseline=${BASELINE} \
    --current=${VERSION} \
    --analysis=docs/apex_core/benchmark/analysis.json \
    --output=docs/apex_core/benchmark/report
```

### GitHub Pages 갱신
보고서 HTML이 변경되면 gh-pages 브랜치도 갱신해야 한다:
```bash
git checkout gh-pages
git checkout main -- docs/apex_core/benchmark/architecture_comparison.html \
                     docs/apex_core/benchmark/report/benchmark_report.html
cp docs/apex_core/benchmark/architecture_comparison.html benchmark/index.html
cp docs/apex_core/benchmark/report/benchmark_report.html benchmark/report.html
git add benchmark/
git commit -m "deploy: benchmark reports update"
git push origin gh-pages
git checkout main  # 또는 원래 브랜치
```

## Affinity 벤치마크

### `--affinity` 플래그

Integration 벤치마크(architecture_comparison, cross_core_latency, cross_core_message_passing)에 `--affinity=on` 플래그를 지정하면 물리 코어에 1:1 핀닝하여 실행한다.

```bash
# Affinity OFF (기본)
apex-agent queue benchmark apex_core/bin/release/bench_architecture_comparison.exe \
    --benchmark_format=json --benchmark_out=result.json

# Affinity ON
apex-agent queue benchmark apex_core/bin/release/bench_architecture_comparison.exe \
    --affinity=on --benchmark_format=json --benchmark_out=result_affinity.json
```

- `--affinity=on`: `CpuTopology::discover()` → 물리 코어 수만큼 `CoreAssignment` 구성
- 물리 코어 수 초과 워커는 핀닝하지 않음 (OS 자유 스케줄링) — 실제 서버 동작과 일치
- micro 벤치마크는 단일 스레드이므로 affinity 무관

### 원클릭 실행 스크립트

```powershell
powershell -ExecutionPolicy Bypass -File apex_tools/benchmark/run_all_benchmarks.ps1 [-Version v0.6.5.0] [-Hardware i7-14700-20C28T]
```

빌드 → micro 9개 → integration OFF 5개 → integration ON 3개 순차 실행. 결과는 `docs/apex_core/benchmark/{version}/{hardware}/data/`에 저장.

## 향후 실측 계획

- **Linux Docker**: 풀 인프라(Gateway + Kafka + Redis + PostgreSQL) 기반 실측 — Windows 환경 결과의 최종 검증
- **NUMA 멀티소켓**: 실제 NUMA 효과 측정 (현재 단일 소켓 환경에서는 미검증)
- **lock-free SessionMap**: `boost::concurrent_flat_map`으로 교체 후 io_context 병목 결정적 검증
