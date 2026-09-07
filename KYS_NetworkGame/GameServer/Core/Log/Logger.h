#pragma once

#include "Core/Thread/SpinLock.h"

#include <Windows.h>

#include <stdio.h>
#include <utility>

namespace KYS
{
	namespace GAMESERVER
	{
		namespace LOG
		{
			// 로그 중요도. 숫자가 작을수록 심각하다.
			//   필터는 "설정 레벨보다 덜 중요한(숫자가 큰) 로그를 버린다" - 기본 LL_INFO(LL_DEBUG만 억제).
			enum class LogLevel : int
			{
				LL_FATAL = 0,   // 프로세스가 계속 갈 수 없는 결함. 큐를 거치지 않고 호출 스레드가 파일에 직접 쓴다(크래시 직전 유실 방지)
				LL_ERROR = 1,   // 기능 실패 (큐 경유 + 해당 배치 즉시 디스크 flush)
				LL_WARN = 2,    // 이상 징후 / 보안 이벤트 (로그인 실패, 연결 거부 등)
				LL_INFO = 3,    // 운영 사건 (부팅, 로그인, 이탈, 채널 이동 등) - 기본 레벨
				LL_DEBUG = 4    // 개발 진단 (기본 억제 - SetLogLevel 로 열어야 기록)
			};

			// 로그 파일 채널. 실무 공통 패턴(운영 로그 / 유저 행동 로그 분리)대로 파일을 나눈다.
			enum class LogChannel : int
			{
				SERVER = 0,   // 수명(부팅/종료) / 장애(DB, 워치독) / 보안(거부, 실패) -> <app>_<pid>_server_*.log
				USER = 1      // 유저 lifecycle (로그인/로그아웃, 캐릭터 생성/삭제, 채널 이동, kick) -> <app>_<pid>_user_*.log
			};

			static const int LOG_CHANNEL_COUNT = 2;

			static const int LOG_TAG_MAX = 16;             // 태그(boot/db/login 등) 최대 글자 수 (종단 포함)
			static const int LOG_TEXT_MAX = 512;           // 본문 최대 글자 수 (초과분은 절단 - CRT 함정을 밟지 않는다)
			static const int LOG_LINE_MAX = LOG_TEXT_MAX + 96;   // 시각/레벨/태그 접두를 붙인 완성 한 줄 상한
			static const int LOG_QUEUE_CAPACITY = 4096;    // 큐 슬롯 수 (1000봇 동시 이탈 버스트 흡수용 - 가득 차면 새 로그를 버리고 카운트)
			static const int LOG_DRAIN_BATCH = 512;        // 로거 스레드가 한 번에 꺼내는 최대 레코드 수 (락 보유 시간 상한)
			static const DWORD LOG_FLUSH_INTERVAL_MS = 500;  // 신호가 없어도 이 주기로 깨어나 잔량을 쓴다
			static const int LOG_FATAL_FILE_TRY = 1024;    // FATAL 이 파일 락을 시도하는 유한 횟수 (실패 시 파일 포기 - abort 도달을 막지 않는다)

			// 로그 한 건 (고정 크기 레코드). 생산자가 스택 로컬에 완성한 뒤 락 안에서 통째로 큐에 복사(commit)한다
			// - 링에는 완성된 레코드만 존재하므로 조립 중 슬롯을 소비자가 읽는 레이스가 구조적으로 없다.
			struct LogRecord
			{
				SYSTEMTIME ts;                  // 생산 시점 기록 (큐 대기 시간과 무관한 사건 시각)
				LogLevel   level;
				LogChannel channel;
				wchar_t    tag[LOG_TAG_MAX];
				wchar_t    text[LOG_TEXT_MAX];
			};

			// 비동기 로거 (lock 기반).
			//   - 생산자(채널/DB/워커/main 등 아무 스레드): 레벨 필터 -> 스택 로컬 조립 -> 락 안 memcpy commit -> 이벤트 신호. 파일 I/O 를 기다리지 않는다.
			//   - 소비자(전용 로거 스레드 1개): 배치로 꺼내 UTF-8 변환 후 채널별 파일에 쓰고, 설정 레벨 이상은 콘솔에도 병행 출력.
			//   - 큐가 가득 차면 새 로그를 버리고 GetDropCount 로 관측한다 (생산자는 절대 기다리지 않는다 - tick 스레드 보호).
			//   - LL_FATAL 은 큐를 우회해 호출 스레드가 파일에 직접 동기 기록(유한 락 시도 - 실패해도 콘솔 출력과 abort 진행을 막지 않음).
			//   - 라이브러리(GameServer.lib) 내부 코드는 이 로거를 호출하지 않는다. 위치만 공용 라이브러리고 호출은 전부 앱 몫이다.
			class Logger
			{
			public:
				static Logger& GetInstance();

				Logger(const Logger&) = delete;
				Logger& operator=(const Logger&) = delete;

				// Logs\<appName>_<pid>_server_YYYYMMDD_HHMMSS.log / ..._user_... 두 파일을 열고 로거 스레드를 시작한다.
				//   파일을 못 열면 콘솔 전용으로 강등해서라도 동작한다 (반환 false = 강등 상태).
				bool Initialize(const wchar_t* appName);

				// 새 로그 접수를 막고(이후 호출은 콘솔로만) 큐 잔량을 전부 파일에 쓴 뒤 로거 스레드를 정리한다.
				//   모든 로그 생산 스레드(채널/DB/링크 등)를 join 한 다음에 불러야 종료 로그가 유실되지 않는다.
				void Shutdown();

				void SetLogLevel(LogLevel level);             // 이 레벨보다 덜 중요한 로그는 접수 자체를 버린다 (기본 LL_INFO)
				void SetConsoleEchoMinLevel(LogLevel level);  // 콘솔 병행 출력의 최소 중요도 (모니터 모드 중에는 LL_WARN 으로 올려 화면 간섭을 줄인다)

				// 로그 한 건 기록. tag 는 짧은 분류어(boot/db/login/leave 등), format 은 wprintf 계열 서식.
				//   레벨 필터가 포맷보다 앞이라 꺼진 로그는 비용이 없다. 본문이 LOG_TEXT_MAX 를 넘으면 절단하고 카운트한다.
				template<typename... Args>
				void Log(LogChannel channel, LogLevel level, const wchar_t* tag, const wchar_t* format, Args&&... args)
				{
					if (static_cast<int>(level) > m_logLevel) { return; }   // 필터가 포맷/시각조회보다 앞 (꺼진 로그 = 비용 0)

					LogRecord record;
					BuildRecordHeader(record, channel, level, tag);
					const int written = ::_snwprintf_s(record.text, LOG_TEXT_MAX, _TRUNCATE, format, std::forward<Args>(args)...);
					if (written < 0) { MarkTruncated(record); }             // 절단 마커 + 카운터 (invalid-parameter 핸들러 경로를 밟지 않는다)

					Dispatch(record);                                       // FATAL = 직접 기록 / 그 외 = 큐 commit
				}

				// 크래시 직후 best-effort 큐 배출. CrashDump 전용 덤프 스레드가 덤프(.dmp) 확보 후 SEH 가드 안에서 부른다.
				//   락을 못 잡으면(락 쥔 채 죽은 스레드 가능) 즉시 포기한다 - 어떤 경우에도 매달리지 않는다.
				void EmergencyFlush();

				UINT64 GetDropCount() const;        // 큐 가득으로 버린 로그 수 (0 이 정상 - 지표로 표출)
				UINT64 GetTruncatedCount() const;   // 본문 절단이 일어난 로그 수
				UINT64 GetWriteFailCount() const;   // 파일 WriteFile 실패/부분 쓰기 수 (디스크 풀 등 - "로그가 안 남는 장애" 의 가시화)

			private:
				Logger();
				~Logger();

				void BuildRecordHeader(LogRecord& record, LogChannel channel, LogLevel level, const wchar_t* tag);
				void MarkTruncated(LogRecord& record);
				void Dispatch(LogRecord& record);
				void PushToQueue(LogRecord& record);
				void WriteFatalDirect(LogRecord& record);

				void WriterLoop();                                     // 로거 스레드 본체
				static unsigned __stdcall WriterThreadEntry(void* self);
				int  DrainBatch(LogRecord* outBatch, int maxCount);    // 큐 락 안에서 배치 복사 + read 인덱스 전진, 꺼낸 수 반환
				void WriteBatchToFiles(LogRecord* batch, int count);   // 배치를 콘솔(레벨 게이트) + 채널별 파일로
				void AppendLineUtf8(LogChannel channel, const wchar_t* line, int lineLen);   // 변환 버퍼에 누적
				void FlushUtf8Buffers(bool forceDiskFlush);            // 채널별 누적분을 파일 락 안에서 WriteFile
				int  FormatLine(const LogRecord& record, wchar_t* outLine) const;            // "[시각][레벨][태그] 본문" 한 줄 조립

				bool OpenChannelFile(LogChannel channel, const wchar_t* appName, DWORD pid, const SYSTEMTIME& bootTime);
				void CloseChannelFiles();

			private:
				// ---- 큐 (생산자 다수 / 소비자 = 로거 스레드 1) ----
				THREAD::SpinLock m_queueLock;       // write/read 인덱스 + 슬롯 commit 보호 (보유 구간 = memcpy 뿐)
				LogRecord* m_records;               // 링 슬롯 (Initialize 에서 확보, LOG_QUEUE_CAPACITY 개)
				int m_writeIndex;
				int m_readIndex;
				int m_pendingCount;                 // 큐에 쌓인 레코드 수
				UINT64 m_dropCount;                 // 가득 참으로 버린 수 (락 안 증가, 밖에서 근사 read)
				volatile LONG64 m_truncatedCount;   // 절단 발생 수 (생산자 다수가 락 밖에서 만짐 - Interlocked 증가)
				volatile LONG64 m_writeFailCount;   // 파일 쓰기 실패/부분 쓰기 수 (로거 스레드 + FATAL 크래시 스레드가 만짐 - Interlocked)

				// ---- 로거 스레드 ----
				HANDLE m_writerThread;
				HANDLE m_wakeEvent;                 // push 시 신호 (auto-reset) - 없어도 LOG_FLUSH_INTERVAL_MS 주기로 깨어남
				volatile LONG m_stopRequested;      // Shutdown -> 로거 스레드 탈출 신호
				LogRecord* m_drainBuffer;           // 로거 스레드 전용 배치 버퍼 (LOG_DRAIN_BATCH 개)

				// ---- 파일 (채널 2개) ----
				THREAD::SpinLock m_fileLock;        // 파일 핸들 보호 (로거 스레드 write / FATAL 직접 write / EmergencyFlush 가 공유)
				HANDLE m_file[LOG_CHANNEL_COUNT];
				char* m_utf8Buffer[LOG_CHANNEL_COUNT];   // 배치 변환 누적 버퍼 (로거 스레드 전용)
				int m_utf8Used[LOG_CHANNEL_COUNT];

				// ---- 상태/설정 ----
				volatile LONG m_running;            // Initialize 완료 ~ Shutdown 시작 (false 면 큐 접수 안 함 - 콘솔 폴백)
				volatile int m_logLevel;            // 접수 필터 (기본 LL_INFO)
				volatile int m_echoMinLevel;        // 콘솔 병행 출력 최소 중요도 (기본 LL_INFO)
			};
		}
	}
}
