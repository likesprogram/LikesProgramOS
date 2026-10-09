/* ReadFailure.cpp
    ReadFailure.hpp 的实现：阶段名与读盘失败诊断行的写法
*/

#include <ReadFailure.hpp>

#include <Print.hpp>
#include <PrintTarget.hpp>

namespace Baleen {
    const char* ReadFailPhaseName(uint32_t phase) {
        switch (static_cast<ReadFailPhase>(phase)) {
            case ReadFailPhase::None: return "None";
            case ReadFailPhase::Reject: return "Reject";
            case ReadFailPhase::BatchEdd: return "BatchEdd";
            case ReadFailPhase::SingleEdd: return "SingleEdd";
            case ReadFailPhase::SingleChs: return "SingleChs";
            case ReadFailPhase::Window: return "Window";
        }
        return "Unknown";
    }

    void WriteReadFailureLine(const ReadFailureFacts& failure) {
        PrintTargets::WriteTag(PrintTargets::Tag::Continue);
        Print::Write("Lba=");
        Print::WriteHex(failure.lba);
        Print::Write(" Count=");
        Print::WriteHex(failure.count);
        Print::Write(" Ah=");
        Print::WriteHex(failure.status);
        Print::Write(" Phase=");
        Print::Write(ReadFailPhaseName(failure.phase));
        Print::Write("\r\n");
    }
}
