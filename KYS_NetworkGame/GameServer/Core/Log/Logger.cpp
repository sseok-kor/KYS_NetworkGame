#include "pch_gameserver.h"
#include "Core/Log/Logger.h"
#include <string.h>   // wcsncpy_s / wcslen

namespace KYS
{
	namespace GAMESERVER
	{
		namespace LOG
		{
			namespace
			{
				const wchar_t LOG_FOLDER[] = L"Logs";

				// 채널별 파일 이름 조각과 레벨 표기 (한 줄 접두에 쓴다)
				const wchar_t* ChannelName(LogChannel channel)
				{
					return (channel == LogChannel::USER) ? L"user" : L"server";
				}

				const wchar_t* LevelName(LogLevel level)
				{
					switch (level)
					{
					case LogLevel::LL_FATAL: return L"FATAL";
					case LogLevel::LL_ERROR: return L"ERROR";
					case LogLevel::LL_WARN:  return L"WARN";
					case LogLevel::LL_INFO:  return L"INFO";
					default:                 return L"DEBUG";
					}
				}
			}

			Logger& Logger::GetInstance()
			{
				static Logger instance;
				return instance;
			}

			Logger::Logger()
				: m_records(nullptr)
				, m_writeIndex(0)
				, m_readIndex(0)
				, m_pendingCount(0)
				, m_dropCount(0)
				, m_truncatedCount(0)
				, m_writeFailCount(0)
				, m_writerThread(nullptr)
				, m_wakeEvent(nullptr)
				, m_stopRequested(0)
				, m_drainBuffer(nullptr)
				, m_running(0)
				, m_logLevel(static_cast<int>(LogLevel::LL_INFO))
				, m_echoMinLevel(static_cast<int>(LogLevel::LL_INFO))
			{
				for (int i = 0; i < LOG_CHANNEL_COUNT; ++i)
				{
					m_file[i] = INVALID_HANDLE_VALUE;
					m_utf8Buffer[i] = nullptr;
					m_utf8Used[i] = 0;
				}
			}

			Logger::~Logger()
			{
				// 프로세스 종료 시점(정적 소멸)에는 스레드/핸들 정리를 하지 않는다 - 명시적 Shutdown 이 정리 책임.
			}

			bool Logger::Initialize(const wchar_t* appName)
			{
				if (m_running != 0) { return true; }   // 중복 호출 무해

				// 파일이 놓일 폴더부터 확보 (이미 있으면 무해). metrics 파일도 같은 폴더를 쓴다.
				::CreateDirectoryW(LOG_FOLDER, nullptr);

				SYSTEMTIME bootTime;
				::GetLocalTime(&bootTime);
				const DWORD pid = ::GetCurrentProcessId();
				const wchar_t* name = (appName != nullptr) ? appName : L"App";

				bool fileOk = true;
				for (int i = 0; i < LOG_CHANNEL_COUNT; ++i)
				{
					if (OpenChannelFile(static_cast<LogChannel>(i), name, pid, bootTime) == false)
					{
						fileOk = false;
					}
				}
				if (fileOk == false)
				{
					// 파일 없이도 동작은 계속한다 (콘솔 강등) - 로깅 실패가 서버를 멈추게 하지 않는다.
					::wprintf(L"[log][warn] log file open failed - console only (folder: %s)\n", LOG_FOLDER);
				}

				// 이벤트를 버퍼 할당보다 먼저 - 실패 시 해제할 것이 파일뿐이라 정리가 단순하다 (할당 후 실패 = 누수 경로 차단).
				m_wakeEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);   // auto-reset
				if (m_wakeEvent == nullptr)
				{
					::wprintf(L"[log][warn] wake event create failed - console only\n");
					CloseChannelFiles();
					return false;
				}

				m_records = new LogRecord[LOG_QUEUE_CAPACITY];
				m_drainBuffer = new LogRecord[LOG_DRAIN_BATCH];
				for (int i = 0; i < LOG_CHANNEL_COUNT; ++i)
				{
					m_utf8Buffer[i] = new char[static_cast<size_t>(LOG_DRAIN_BATCH) * LOG_LINE_MAX * 3];
					m_utf8Used[i] = 0;
				}

				m_stopRequested = 0;
				unsigned threadId = 0;
				m_writerThread = reinterpret_cast<HANDLE>(::_beginthreadex(nullptr, 0, &WriterThreadEntry, this, 0, &threadId));
				if (m_writerThread == nullptr)
				{
					::wprintf(L"[log][warn] writer thread create failed - console only\n");
					delete[] m_records;      m_records = nullptr;      // 스레드가 없으니 버퍼도 회수 (누수 방지)
					delete[] m_drainBuffer;  m_drainBuffer = nullptr;
					for (int i = 0; i < LOG_CHANNEL_COUNT; ++i)
					{
						delete[] m_utf8Buffer[i];
						m_utf8Buffer[i] = nullptr;
					}
					::CloseHandle(m_wakeEvent);
					m_wakeEvent = nullptr;
					CloseChannelFiles();
					return false;
				}

				m_running = 1;
				return fileOk;
			}

			void Logger::Shutdown()
			{
				if (m_writerThread == nullptr) { return; }

				// 새 접수를 먼저 막는다 (락 안에서 내려야 진행 중인 commit 과 어긋나지 않는다). 이후 Log 호출은 콘솔로만 나간다.
				m_queueLock.Lock();
				m_running = 0;
				m_queueLock.UnLock();

				// 로거 스레드가 잔량을 전부 쓰고 나오게 한다 (drain 후 탈출).
				m_stopRequested = 1;
				::SetEvent(m_wakeEvent);
				::WaitForSingleObject(m_writerThread, INFINITE);
				::CloseHandle(m_writerThread);
				m_writerThread = nullptr;

				CloseChannelFiles();

				::CloseHandle(m_wakeEvent);
				m_wakeEvent = nullptr;

				delete[] m_records;      m_records = nullptr;
				delete[] m_drainBuffer;  m_drainBuffer = nullptr;
				for (int i = 0; i < LOG_CHANNEL_COUNT; ++i)
				{
					delete[] m_utf8Buffer[i];
					m_utf8Buffer[i] = nullptr;
					m_utf8Used[i] = 0;
				}
			}

			void Logger::SetLogLevel(LogLevel level)
			{
				m_logLevel = static_cast<int>(level);
			}

			void Logger::SetConsoleEchoMinLevel(LogLevel level)
			{
				m_echoMinLevel = static_cast<int>(level);
			}

			UINT64 Logger::GetDropCount() const
			{
				return m_dropCount;   // 락 안에서 증가 / 밖에서 근사 read (지표용)
			}

			UINT64 Logger::GetTruncatedCount() const
			{
				return static_cast<UINT64>(m_truncatedCount);
			}

			UINT64 Logger::GetWriteFailCount() const
			{
				return static_cast<UINT64>(m_writeFailCount);
			}

			void Logger::BuildRecordHeader(LogRecord& record, LogChannel channel, LogLevel level, const wchar_t* tag)
			{
				::GetLocalTime(&record.ts);   // 생산 시점 시각 (큐 대기와 무관)
				record.level = level;
				record.channel = channel;
				::wcsncpy_s(record.tag, LOG_TAG_MAX, (tag != nullptr) ? tag : L"-", _TRUNCATE);
			}

			void Logger::MarkTruncated(LogRecord& record)
			{
				// _TRUNCATE 로 이미 종단은 보장 - 말미에 절단 표시를 남기고 센다.
				record.text[LOG_TEXT_MAX - 3] = L'.';
				record.text[LOG_TEXT_MAX - 2] = L'.';
				record.text[LOG_TEXT_MAX - 1] = L'\0';
				::InterlockedIncrement64(&m_truncatedCount);
			}

			void Logger::Dispatch(LogRecord& record)
			{
				if (record.level == LogLevel::LL_FATAL)
				{
					WriteFatalDirect(record);   // 크래시 임박 - 로거 스레드 생사에 기대지 않는다
					return;
				}
				PushToQueue(record);
			}

			void Logger::PushToQueue(LogRecord& record)
			{
				bool queued = false;

				m_queueLock.Lock();
				if (m_running != 0)
				{
					if (m_pendingCount < LOG_QUEUE_CAPACITY)
					{
						m_records[m_writeIndex] = record;   // 완성본 통째 commit (링에는 완성 레코드만 존재)
						m_writeIndex = (m_writeIndex + 1) % LOG_QUEUE_CAPACITY;
						++m_pendingCount;
						queued = true;
					}
					else
					{
						++m_dropCount;   // 가득 참 - 생산자는 절대 기다리지 않는다 (tick 보호), 유실은 지표로 관측
					}
				}
				m_queueLock.UnLock();

				if (queued)
				{
					::SetEvent(m_wakeEvent);
					return;
				}

				// Shutdown 이후(또는 미초기화) 접수 - 콘솔로만 남긴다 (종료 저장 중 오류 등이 침묵하지 않게).
				if (m_running == 0 && static_cast<int>(record.level) <= m_echoMinLevel)
				{
					wchar_t line[LOG_LINE_MAX];
					const int len = FormatLine(record, line);
					if (len > 0) { ::wprintf(L"%s\n", line); }
				}
			}

			void Logger::WriteFatalDirect(LogRecord& record)
			{
				wchar_t line[LOG_LINE_MAX];
				const int len = FormatLine(record, line);
				if (len <= 0) { return; }

				// 콘솔 먼저, 락 밖에서 - 파일 락이 어떤 상태든 FATAL 은 최소 콘솔에는 보인다.
				::wprintf(L"%s\n", line);

				// 파일은 유한 시도만 - 락을 쥔 채 멈춘 스레드가 있어도 여기 매달려 abort 도달을 막지 않는다.
				bool locked = false;
				for (int i = 0; i < LOG_FATAL_FILE_TRY; ++i)
				{
					if (m_fileLock.TryLock()) { locked = true; break; }
					::Sleep(0);
				}
				if (locked == false) { return; }   // 파일 포기 (콘솔에는 이미 남음)

				const int channelIndex = static_cast<int>(record.channel);
				HANDLE file = m_file[channelIndex];
				if (file != INVALID_HANDLE_VALUE)
				{
					char utf8[LOG_LINE_MAX * 3 + 2];
					int bytes = ::WideCharToMultiByte(CP_UTF8, 0, line, len, utf8, sizeof(utf8) - 2, nullptr, nullptr);
					if (bytes > 0)
					{
						utf8[bytes] = '\r';
						utf8[bytes + 1] = '\n';
						DWORD written = 0;
						const BOOL ok = ::WriteFile(file, utf8, static_cast<DWORD>(bytes) + 2, &written, nullptr);
						if (ok == FALSE || written != static_cast<DWORD>(bytes) + 2)
						{
							::InterlockedIncrement64(&m_writeFailCount);   // 파일 실패 - 콘솔에는 위에서 이미 남았다
						}
						::FlushFileBuffers(file);   // 크래시 직전 - OS 캐시까지 밀어낸다
					}
				}
				m_fileLock.UnLock();
			}

			unsigned __stdcall Logger::WriterThreadEntry(void* self)
			{
				static_cast<Logger*>(self)->WriterLoop();
				return 0;
			}

			void Logger::WriterLoop()
			{
				for (;;)
				{
					::WaitForSingleObject(m_wakeEvent, LOG_FLUSH_INTERVAL_MS);

					for (;;)
					{
						const int count = DrainBatch(m_drainBuffer, LOG_DRAIN_BATCH);
						if (count == 0) { break; }
						WriteBatchToFiles(m_drainBuffer, count);
					}

					// stop 신호 후에도 위 drain 루프가 잔량을 비운 뒤에 탈출한다 (Shutdown 이 접수를 먼저 막으므로 새 유입 없음).
					if (m_stopRequested != 0) { break; }
				}
			}

			int Logger::DrainBatch(LogRecord* outBatch, int maxCount)
			{
				m_queueLock.Lock();

				int count = (m_pendingCount < maxCount) ? m_pendingCount : maxCount;
				if (count > 0)
				{
					// 링 경계에서 최대 두 구간으로 나뉜다 - 락 보유 구간은 이 memcpy 뿐.
					const int firstSpan = LOG_QUEUE_CAPACITY - m_readIndex;
					const int firstCopy = (count < firstSpan) ? count : firstSpan;
					::memcpy(outBatch, &m_records[m_readIndex], sizeof(LogRecord) * firstCopy);
					if (count > firstCopy)
					{
						::memcpy(outBatch + firstCopy, &m_records[0], sizeof(LogRecord) * (count - firstCopy));
					}
					m_readIndex = (m_readIndex + count) % LOG_QUEUE_CAPACITY;
					m_pendingCount -= count;
				}

				m_queueLock.UnLock();
				return count;
			}

			void Logger::WriteBatchToFiles(LogRecord* batch, int count)
			{
				bool needDiskFlush = false;
				wchar_t line[LOG_LINE_MAX];

				for (int i = 0; i < count; ++i)
				{
					const LogRecord& record = batch[i];
					const int len = FormatLine(record, line);
					if (len <= 0) { continue; }

					// 콘솔 병행 출력 (로거 스레드에서 - 호출자는 콘솔 왕복을 모른다)
					if (static_cast<int>(record.level) <= m_echoMinLevel)
					{
						::wprintf(L"%s\n", line);
					}

					AppendLineUtf8(record.channel, line, len);

					if (record.level == LogLevel::LL_ERROR || record.level == LogLevel::LL_FATAL)
					{
						needDiskFlush = true;   // 오류가 낀 배치는 OS 캐시까지 즉시 밀어낸다
					}
				}

				FlushUtf8Buffers(needDiskFlush);
			}

			void Logger::AppendLineUtf8(LogChannel channel, const wchar_t* line, int lineLen)
			{
				const int channelIndex = static_cast<int>(channel);
				if (m_file[channelIndex] == INVALID_HANDLE_VALUE) { return; }   // 콘솔 강등 상태 - 파일 누적 생략

				char* buffer = m_utf8Buffer[channelIndex];
				const int capacity = LOG_DRAIN_BATCH * LOG_LINE_MAX * 3;

				// 남은 공간이 최악 변환 크기보다 작으면 먼저 비운다 (배치 내 중간 flush).
				if (m_utf8Used[channelIndex] + lineLen * 3 + 2 > capacity)
				{
					FlushUtf8Buffers(false);
				}

				const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, line, lineLen,
					buffer + m_utf8Used[channelIndex], capacity - m_utf8Used[channelIndex] - 2, nullptr, nullptr);
				if (bytes > 0)
				{
					m_utf8Used[channelIndex] += bytes;
					buffer[m_utf8Used[channelIndex]] = '\r';
					buffer[m_utf8Used[channelIndex] + 1] = '\n';
					m_utf8Used[channelIndex] += 2;
				}
			}

			void Logger::FlushUtf8Buffers(bool forceDiskFlush)
			{
				for (int i = 0; i < LOG_CHANNEL_COUNT; ++i)
				{
					if (m_utf8Used[i] <= 0) { continue; }

					m_fileLock.Lock();
					HANDLE file = m_file[i];
					if (file != INVALID_HANDLE_VALUE)
					{
						DWORD written = 0;
						const BOOL ok = ::WriteFile(file, m_utf8Buffer[i], static_cast<DWORD>(m_utf8Used[i]), &written, nullptr);
						if (ok == FALSE || written != static_cast<DWORD>(m_utf8Used[i]))
						{
							// 디스크 풀 등 - 배치 포기하되 반드시 센다 ("로그가 안 남는 장애" 를 지표 0 인 채 침묵시키지 않는다).
							::InterlockedIncrement64(&m_writeFailCount);
						}
						if (forceDiskFlush)
						{
							::FlushFileBuffers(file);
						}
					}
					m_fileLock.UnLock();

					m_utf8Used[i] = 0;
				}
			}

			int Logger::FormatLine(const LogRecord& record, wchar_t* outLine) const
			{
				const int len = ::_snwprintf_s(outLine, LOG_LINE_MAX, _TRUNCATE,
					L"[%04u-%02u-%02u %02u:%02u:%02u.%03u][%s][%s] %s",
					record.ts.wYear, record.ts.wMonth, record.ts.wDay,
					record.ts.wHour, record.ts.wMinute, record.ts.wSecond, record.ts.wMilliseconds,
					LevelName(record.level), record.tag, record.text);
				return (len > 0) ? len : 0;
			}

			void Logger::EmergencyFlush()
			{
				// 크래시 문맥 - 락을 못 잡으면 즉시 포기한다 (락 쥔 채 죽은 스레드에 매달리지 않는다).
				// 한 건씩 "큐 락 -> 복사 -> 락 해제 -> 파일" 순서라 큐 락과 파일 락을 동시에 쥐지 않는다.
				wchar_t line[LOG_LINE_MAX];
				char utf8[LOG_LINE_MAX * 3 + 2];

				for (;;)
				{
					if (m_queueLock.TryLock() == false) { return; }
					if (m_pendingCount <= 0 || m_records == nullptr)
					{
						m_queueLock.UnLock();
						break;
					}
					LogRecord record = m_records[m_readIndex];
					m_readIndex = (m_readIndex + 1) % LOG_QUEUE_CAPACITY;
					--m_pendingCount;
					m_queueLock.UnLock();

					const int len = FormatLine(record, line);
					if (len <= 0) { continue; }

					const int channelIndex = static_cast<int>(record.channel);
					if (m_file[channelIndex] == INVALID_HANDLE_VALUE) { continue; }
					if (m_fileLock.TryLock() == false) { return; }
					HANDLE file = m_file[channelIndex];
					if (file != INVALID_HANDLE_VALUE)
					{
						const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, line, len, utf8, sizeof(utf8) - 2, nullptr, nullptr);
						if (bytes > 0)
						{
							utf8[bytes] = '\r';
							utf8[bytes + 1] = '\n';
							DWORD written = 0;
							::WriteFile(file, utf8, static_cast<DWORD>(bytes) + 2, &written, nullptr);
						}
					}
					m_fileLock.UnLock();
				}

				// 남긴 것을 OS 캐시까지 밀어낸다 (역시 유한 시도).
				if (m_fileLock.TryLock())
				{
					for (int i = 0; i < LOG_CHANNEL_COUNT; ++i)
					{
						if (m_file[i] != INVALID_HANDLE_VALUE) { ::FlushFileBuffers(m_file[i]); }
					}
					m_fileLock.UnLock();
				}
			}

			bool Logger::OpenChannelFile(LogChannel channel, const wchar_t* appName, DWORD pid, const SYSTEMTIME& bootTime)
			{
				wchar_t path[MAX_PATH];
				::_snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%s_%u_%s_%04u%02u%02u_%02u%02u%02u.log",
					LOG_FOLDER, appName, pid, ChannelName(channel),
					bootTime.wYear, bootTime.wMonth, bootTime.wDay,
					bootTime.wHour, bootTime.wMinute, bootTime.wSecond);

				// CREATE_NEW - 동명 파일을 무경고로 비우지 않는다 (앱명+pid+초 단위 시각이라 정상 충돌은 없고,
				//   비정상 충돌이면 열기 실패 -> 호출자의 콘솔 강등 경로가 그대로 받는다).
				HANDLE file = ::CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
					CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (file == INVALID_HANDLE_VALUE) { return false; }

				// UTF-8 BOM - 메모장/에디터가 인코딩을 바로 알아보게.
				const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
				DWORD written = 0;
				::WriteFile(file, bom, 3, &written, nullptr);

				m_file[static_cast<int>(channel)] = file;
				return true;
			}

			void Logger::CloseChannelFiles()
			{
				m_fileLock.Lock();
				for (int i = 0; i < LOG_CHANNEL_COUNT; ++i)
				{
					if (m_file[i] != INVALID_HANDLE_VALUE)
					{
						::FlushFileBuffers(m_file[i]);
						::CloseHandle(m_file[i]);
						m_file[i] = INVALID_HANDLE_VALUE;
					}
				}
				m_fileLock.UnLock();
			}
		}
	}
}
