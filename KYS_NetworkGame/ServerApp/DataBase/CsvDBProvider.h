#pragma once
#include "IDBProvider.h"
#include "DBResult.h"      // DBResult (캐시 값 - 완전 타입 필요)
#include <cstdio>          // FILE (WriteRow 인자), CRT 파일 IO
#include <unordered_map>   // 인메모리 캐시 (이름 -> 캐릭터)
#include <string>          // std::wstring 키 (AccountManager 패턴 정합, wchar_t 기반)

namespace KYS
{
    namespace SERVERAPP
    {
        // CSV 백엔드 stub. 매 조회/저장마다 디스크를 O(file) 스캔/재작성하던 것을 인메모리 해시 캐시로 바꿔
        //   DB 스레드를 빠르게 한다(부하 적체=in-flight 갭 축소). 부팅 첫 접근 시 characters.csv를 캐시에 1회 적재 ->
        //   Load=O(1) 해시 조회 / SaveFull=O(1) 메모리 갱신. 디스크는 종료 시 1회 flush(스트레스엔 충분, 진짜 내구성은 RDB).
        class CsvDBProvider : public IDBProvider
        {
        public:
            CsvDBProvider();
            virtual ~CsvDBProvider();   // 종료 시 캐시를 characters.csv로 flush

            // IDBProvider 작업 함수 구현 (override).
            bool Load(const wchar_t* key, DBResult& out) override;
            void SaveFull(const DBResult& snapshot) override;
            void SaveDelta(const DBResult& snapshot, UINT32 dirtyMask) override;

            // 트랜잭션 - CSV 는 단일 파일 캐시라 트랜잭션 개념이 없어 no-op(인터페이스 충족용).
            void Begin() override;
            void Commit() override;
            void Rollback() override;

            // [!] 복사/이동은 base IDBProvider 가 4줄 =delete -> 파생 자동 차단 (재선언 불요).

        private:
            void EnsureLoaded();   // 첫 접근 시 characters.csv를 캐시로 1회 적재 (DB 스레드 전용 - 락 불요)
            void Flush();          // 캐시를 characters.csv로 통째 기록 (종료 시, 변경 있을 때만)
            // CSV 한 행 쓰기 (컬럼 순서를 한곳에서 정함 - 읽기(swscanf_s)와 순서 대칭).
            void WriteRow(FILE* fp, const DBResult& r) const;

            std::unordered_map<std::wstring, DBResult> m_cache;   // 이름 -> 캐릭터 (인메모리 source of truth)
            bool m_loaded;   // characters.csv 적재 완료 여부 (lazy 1회)
            bool m_dirty;    // 마지막 flush 이후 변경 여부 (변경 없으면 flush skip)
        };
    }
}
