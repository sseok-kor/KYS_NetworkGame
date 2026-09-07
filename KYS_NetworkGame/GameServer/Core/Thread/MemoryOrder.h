#pragma once

namespace KYS
{
    namespace GAMESERVER
    {
        namespace THREAD
        {

#if defined(_M_X64) || defined(_M_IX86)
            // ===== Windows x86/x64 (메모리 순서가 강한 구조, TSO) =====

            // 락을 잡는 쪽에서 쓰는 읽기 - 이 읽기보다 뒤의 작업이 앞으로 새지 않게 막는다 (acquire).
            //   p : 읽어올 대상 주소
            template<typename T>
            inline T LoadAcquire(volatile T* p)
            {
                return *p;  // x86은 메모리 순서가 강해 일반 읽기가 이미 acquire 효과
            }

            // 락을 푸는 쪽에서 쓰는 쓰기 - 이 쓰기보다 앞의 작업이 뒤로 밀리지 않게 막는다 (release).
            //   p   : 쓸 대상 주소
            //   val : 쓸 값
            template<typename T>
            inline void StoreRelease(volatile T* p, T val)
            {
                *p = val;   // x86은 메모리 순서가 강해 일반 쓰기가 이미 release 효과
            }

            // dest가 comparand와 같으면 exchange로 원자적 교체, 직전 값 반환. 앞뒤 메모리 순서 모두 보장.
            //   dest      : 비교/교체 대상 주소
            //   exchange  : 같을 때 새로 넣을 값
            //   comparand : 기대하던 현재 값
            inline LONG64 CompareExchangeAcqRel(
                volatile LONG64* dest, LONG64 exchange, LONG64 comparand)
            {
                return ::InterlockedCompareExchange64(dest, exchange, comparand);
                // LOCK CMPXCHG = 앞뒤 재배치를 모두 막는 full barrier (Store-Load 차단까지)
            }

            // dest 값을 value로 원자적으로 바꾸고 직전 값 반환.
            //   dest  : 교체 대상 주소
            //   value : 새로 넣을 값
            inline LONG64 ExchangeAcqRel(
                volatile LONG64* dest, LONG64 value)
            {
                return ::InterlockedExchange64(dest, value);
            }

            // 앞뒤 모든 메모리 작업의 순서를 강제로 가른다 (재배치 차단).
            inline void FullBarrier()
            {
                ::MemoryBarrier();  // x86: mfence
            }

#elif defined(_M_ARM64)
            // ===== ARM64 (메모리 순서가 약해 명시적 명령이 필요한 구조) =====

            // 락을 잡는 쪽에서 쓰는 읽기 - 이 읽기보다 뒤의 작업이 앞으로 새지 않게 막는다 (acquire).
            //   p : 읽어올 대상 주소
            template<typename T>
            inline T LoadAcquire(volatile T* p)
            {
                return (T)__ldar64((volatile __int64*)p);
                // LDAR (Load-Acquire Register)
            }

            // 락을 푸는 쪽에서 쓰는 쓰기 - 이 쓰기보다 앞의 작업이 뒤로 밀리지 않게 막는다 (release).
            //   p   : 쓸 대상 주소
            //   val : 쓸 값
            template<typename T>
            inline void StoreRelease(volatile T* p, T val)
            {
                __stlr64((volatile __int64*)p, (__int64)val);
                // STLR (Store-Release Register)
            }

            // dest가 comparand와 같으면 exchange로 원자적 교체, 직전 값 반환. 앞뒤 메모리 순서 모두 보장.
            //   dest      : 비교/교체 대상 주소
            //   exchange  : 같을 때 새로 넣을 값
            //   comparand : 기대하던 현재 값
            inline LONG64 CompareExchangeAcqRel(
                volatile LONG64* dest, LONG64 exchange, LONG64 comparand)
            {
                return ::InterlockedCompareExchange64(dest, exchange, comparand);
                // 접미사 없는 full-barrier 버전 = acquire+release 둘 다. 이 함수 하나가
                // Enqueue 링크 CAS(노드 내용 write를 publish -> release 필요)와 Dequeue head
                // CAS(acquire 필요)를 겸하므로 더 강한 쪽으로 맞춘다. Windows ARM64의 _acq 변종은
                // acquire만 걸어 release가 빠지고, 그러면 링크는 새 값인데 노드 내용은 옛 값으로
                // 보이는 torn read가 날 수 있다(이름 AcqRel과 어긋남). x64 LOCK CMPXCHG와 동일 의미.
            }

            // dest 값을 value로 원자적으로 바꾸고 직전 값 반환.
            //   dest  : 교체 대상 주소
            //   value : 새로 넣을 값
            inline LONG64 ExchangeAcqRel(
                volatile LONG64* dest, LONG64 value)
            {
                return ::InterlockedExchange64(dest, value);
                // 위 CAS와 같은 이유로 접미사 없는 full-barrier 버전(_acq 아님)을 쓴다.
            }

            // 앞뒤 모든 메모리 작업의 순서를 강제로 가른다 (재배치 차단).
            inline void FullBarrier()
            {
                __dmb(_ARM64_BARRIER_ISH);
                // DMB ISH (Inner Shareable Domain Memory Barrier)
            }

#else
#error "Unsupported platform: only Windows x86/x64 and ARM64 are supported"
#endif

        }
    }
}
