#pragma once

#include <cstddef>

namespace apex::core {

/// CoreAllocator concept — 메모리 할당기의 기본 인터페이스 계약.
///
/// 모든 apex 할당기(BumpAllocator, ArenaAllocator, SlabAllocator)가 만족해야 하는
/// 최소 요구사항을 정의한다.
///
/// allocate() 시그니처:
///   void* allocate(size_t size, size_t align = alignof(std::max_align_t))
///
/// NOTE (MSVC): std::aligned_alloc은 MSVC에서 미지원.
/// 내부 chunk 할당 시 malloc의 16바이트 정렬 보장을 활용하거나
/// _aligned_malloc / _aligned_free를 사용할 것.
template <typename T>
concept CoreAllocator = requires(T a, void* ptr) {
    { a.allocate(std::size_t{}, std::size_t{}) } -> std::same_as<void*>;
    { a.owns(ptr) }       -> std::same_as<bool>;
    { a.used_bytes() }    -> std::convertible_to<std::size_t>;
    { a.capacity() }      -> std::convertible_to<std::size_t>;
};

/// Freeable concept — 개별 해제가 가능한 할당기.
///
/// CoreAllocator를 만족하면서 추가로 deallocate()를 제공한다.
/// SlabAllocator처럼 개별 슬롯 반납이 가능한 할당기에 사용한다.
template <typename T>
concept Freeable = CoreAllocator<T> && requires(T a, void* ptr) {
    { a.deallocate(ptr) } -> std::same_as<void>;
};

/// Resettable concept — 일괄 초기화가 가능한 할당기.
///
/// CoreAllocator를 만족하면서 추가로 reset()을 제공한다.
/// BumpAllocator/ArenaAllocator처럼 전체를 한 번에 되돌릴 수 있는 할당기에 사용한다.
template <typename T>
concept Resettable = CoreAllocator<T> && requires(T a) {
    { a.reset() } -> std::same_as<void>;
};

} // namespace apex::core
