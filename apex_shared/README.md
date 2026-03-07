# apex_shared

서비스 간 공유 리소스.

## 구조

- `schemas/` — 공유 FlatBuffers 스키마 (.fbs)
- `lib/` — 공유 C++ 코드 (STATIC 라이브러리)
  - `include/apex/shared/` — 공유 헤더
  - `src/` — 구현

## FlatBuffers 네임스페이스 규칙

| 레이어 | 네임스페이스 | 위치 |
|--------|-------------|------|
| 프레임워크 | `apex.messages` | apex_core/schemas/ |
| 서비스 공유 | `apex.shared.<domain>` | apex_shared/schemas/ |

## 스키마 추가 방법

1. `schemas/` 에 `.fbs` 파일 작성 (namespace: `apex.shared.<domain>`)
2. `CMakeLists.txt`의 `APEX_SHARED_SCHEMAS` 리스트에 경로 추가
3. 빌드 시 `generated/<name>_generated.h` 자동 생성

## 서비스에서 사용

```cmake
target_link_libraries(my_service PRIVATE apex::shared)
```

```cpp
#include "generated/<schema>_generated.h"
```
