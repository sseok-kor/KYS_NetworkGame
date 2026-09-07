#include "pch_serverapp.h"
#include "NameDirectoryJobs.h"
#include "SharedServicesThread.h"

namespace KYS
{
    namespace SERVERAPP
    {
        void RegisterNameJob::Execute()
        {
            SharedServicesThread::GetInstance().RegisterName(m_name, m_sid);     // SST 스레드 단독 write
        }
        void UnregisterNameJob::Execute()
        {
            SharedServicesThread::GetInstance().UnregisterName(m_name, m_sid);   // sid 일치 시에만 erase
        }
    }
}