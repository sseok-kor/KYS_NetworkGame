#include "pch_serverapp.h"
#include "CsvDBProvider.h"
#include "../../GameCommon/GameDefines.h"   // WHISPER_NAME_MAX
#include <cwchar>                           // fgetws/fwprintf/swscanf_s/wcscpy_s
#include <cstdio>                           // _wfopen_s/_wremove/_wrename/fclose

namespace
{
    // CSV 파일 경로 (이 파일 안에서만 씀, 설정 불필요한 stub). 나중에 RDB 도입 시 연결 문자열로 대체.
    const wchar_t* const CSV_PATH = L"characters.csv";
    const wchar_t* const CSV_TMP = L"characters.csv.tmp";   // flush용 임시 파일 (rename 으로 한 번에 교체 - 중간 크래시 시 원본 보존)

    const int DB_LINE_MAX = 256;   // CSV 한 줄 최대 wchar (name 16 + 정수 7개 + 콤마/여유)
}

namespace KYS
{
    namespace SERVERAPP
    {
        CsvDBProvider::CsvDBProvider()
            : m_loaded(false)
            , m_dirty(false)
        {
        }

        CsvDBProvider::~CsvDBProvider()
        {
            Flush();   // 종료 시 캐시를 디스크로 1회 기록 (DB 스레드는 main 이 Stop=join 한 뒤라 단독 접근)
        }

        // 첫 접근 때 characters.csv를 캐시로 1회 적재한다. 이후 Load/SaveFull은 디스크를 안 만진다.
        //   DB 스레드 전용(Load/SaveFull 호출자가 DB 스레드) -> 캐시 접근에 락 불요.
        void CsvDBProvider::EnsureLoaded()
        {
            if (m_loaded)
            {
                return;
            }
            m_loaded = true;   // 파일이 없어도 "적재 시도함"으로 표시 (빈 DB)

            FILE* fp = nullptr;
            if (_wfopen_s(&fp, CSV_PATH, L"rt, ccs=UTF-8") != 0 || fp == nullptr)
            {
                return;   // 파일 없음 = 빈 DB (정상, 아직 저장 0)
            }

            wchar_t line[DB_LINE_MAX];
            while (fgetws(line, DB_LINE_MAX, fp) != nullptr)
            {
                DBResult r{};
                wchar_t name[WHISPER_NAME_MAX] = { 0 };

                // width 제한(%15 = WHISPER_NAME_MAX-1, 널 자리 확보) + _s 버퍼 인자로 버퍼 오버런 차단.
                const int matched = swscanf_s(line, L"%15[^,],%d,%d,%d,%d,%d,%d",
                    name, static_cast<unsigned>(WHISPER_NAME_MAX),
                    &r.mapId, &r.x, &r.y, &r.hp, &r.maxHp, &r.mp);
                if (matched != 7)
                {
                    continue;   // 깨진/빈 줄 -> 건너뜀
                }
                wcscpy_s(r.name, WHISPER_NAME_MAX, name);
                r.found = true;
                m_cache[std::wstring(name)] = r;
            }
            fclose(fp);
        }

        // 캐시에서 O(1) 조회 (디스크 스캔 없음). 미발견이면 out.found=false 그대로(호출측이 기본 캐릭터 생성).
        bool CsvDBProvider::Load(const wchar_t* key, DBResult& out)
        {
            out.found = false;
            if (key == nullptr)
            {
                return true;   // 잘못된 키 -> 미발견 처리 (backend 도달 자체는 성공)
            }
            EnsureLoaded();

            std::unordered_map<std::wstring, DBResult>::const_iterator it = m_cache.find(std::wstring(key));
            if (it != m_cache.end())
            {
                // 발견 -> 영속 필드만 복사. sid/chId 등 런타임 라우팅 필드는 호출측(LoadCharacterCommand)이 세팅.
                const DBResult& c = it->second;
                wcscpy_s(out.name, WHISPER_NAME_MAX, c.name);
                out.mapId = c.mapId;
                out.x = c.x;
                out.y = c.y;
                out.hp = c.hp;
                out.maxHp = c.maxHp;
                out.mp = c.mp;
                out.found = true;
            }
            return true;   // backend 도달 성공 (found 여부는 out.found 가 운반)
        }

        // 캐시를 O(1) 갱신 (디스크 재작성 없음). 디스크 반영은 종료 시 Flush 한 번.
        void CsvDBProvider::SaveFull(const DBResult& snapshot)
        {
            EnsureLoaded();
            m_cache[std::wstring(snapshot.name)] = snapshot;   // 이름 키로 갱신 or 추가 (Load는 영속 필드만 읽음)
            m_dirty = true;
        }

        void CsvDBProvider::SaveDelta(const DBResult& snapshot, UINT32 dirtyMask)
        {
            // CSV 는 행 단위라 부분 갱신 이득이 없어 SaveFull 에 위임 (변경분 저장 실익은 RDB 컬럼 UPDATE 에서 발현).
            (void)dirtyMask;   // 현 stub 미사용 (/W4 unreferenced 회피)
            SaveFull(snapshot);
        }

        // 트랜잭션 - CSV 는 트랜잭션 개념이 없다(no-op). RDB provider 만 실제 구현.
        void CsvDBProvider::Begin() {}
        void CsvDBProvider::Commit() {}
        void CsvDBProvider::Rollback() {}

        // 캐시 전체를 characters.csv 로 통째 기록한다 (종료 시 1회, 변경 없으면 skip).
        //   temp 에 쓰고 rename 으로 한 번에 교체 -> 기록 중 크래시 시 원본 보존.
        void CsvDBProvider::Flush()
        {
            if (!m_dirty)
            {
                return;
            }

            FILE* out = nullptr;
            if (_wfopen_s(&out, CSV_TMP, L"wt, ccs=UTF-8") != 0 || out == nullptr)
            {
                return;   // temp 못 열면 기록 포기 (stub)
            }
            for (std::unordered_map<std::wstring, DBResult>::const_iterator it = m_cache.begin(); it != m_cache.end(); ++it)
            {
                WriteRow(out, it->second);
            }
            fclose(out);

            _wremove(CSV_PATH);            // 원본 제거 (없어도 무해, 반환값 무시)
            _wrename(CSV_TMP, CSV_PATH);   // temp -> 원본 (거의 원자적 교체)
            m_dirty = false;
        }

        void CsvDBProvider::WriteRow(FILE* fp, const DBResult& r) const
        {
            // CSV 컬럼 순서 = name,mapId,x,y,hp,maxHp,mp (Load 의 swscanf_s 파싱 순서와 대칭).
            fwprintf(fp, L"%s,%d,%d,%d,%d,%d,%d\n",
                r.name,
                r.mapId, r.x, r.y,
                r.hp, r.maxHp, r.mp);
        }
    }
}
