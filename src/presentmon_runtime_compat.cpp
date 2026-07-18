// Minimal runtime compatibility for the PresentMon PresentData library.
// The vendored PresentMon sources remain untouched; only ABI symbols used by
// PresentData are implemented here.

#include <Windows.h>
#include <eh.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <limits>
#include <sstream>
#include <thread>
#include <typeinfo>
#include <utility>

#include "IntelPresentMon/CommonUtilities/Exception.h"
#include "IntelPresentMon/CommonUtilities/Hash.h"
#include "IntelPresentMon/CommonUtilities/PrecisionWaiter.h"
#include "IntelPresentMon/CommonUtilities/Qpc.h"
#include "IntelPresentMon/CommonUtilities/log/EntryBuilder.h"
#include "IntelPresentMon/CommonUtilities/log/GlobalPolicy.h"
#include "IntelPresentMon/CommonUtilities/log/IChannel.h"
#include "IntelPresentMon/CommonUtilities/str/String.h"
#include "IntelPresentMon/CommonUtilities/win/Handle.h"

namespace pmon::util::hash
{
#if defined(_WIN64)
    size_t HashCombine(size_t left, size_t right) noexcept
    {
        constexpr size_t hash_constant = 0x517c'c1b7'2722'0a95ull;
        return left ^ (right + hash_constant + (left << 6) + (left >> 2));
    }

    size_t HashGuid(const _GUID& guid) noexcept
    {
        return HashCombine(
            static_cast<size_t>(guid.Data1) | (static_cast<size_t>(guid.Data2) << 32) |
                (static_cast<size_t>(guid.Data3) << 48),
            reinterpret_cast<const size_t&>(guid.Data4));
    }
#else
    size_t HashCombine(size_t left, size_t right) noexcept
    {
        constexpr size_t hash_constant = 0x9e37'79b9u;
        return left ^ (right + hash_constant + (left << 6) + (left >> 2));
    }

    size_t HashGuid(const _GUID& guid) noexcept
    {
        const size_t first = guid.Data1;
        const size_t second = static_cast<size_t>(guid.Data2) |
            (static_cast<size_t>(guid.Data3) << 16);
        const size_t third = static_cast<size_t>(guid.Data4[0]) |
            (static_cast<size_t>(guid.Data4[1]) << 8) |
            (static_cast<size_t>(guid.Data4[2]) << 16) |
            (static_cast<size_t>(guid.Data4[3]) << 24);
        const size_t fourth = static_cast<size_t>(guid.Data4[4]) |
            (static_cast<size_t>(guid.Data4[5]) << 8) |
            (static_cast<size_t>(guid.Data4[6]) << 16) |
            (static_cast<size_t>(guid.Data4[7]) << 24);
        return HashCombine(HashCombine(first, second), HashCombine(third, fourth));
    }
#endif
}

namespace pmon::util::str
{
    std::wstring ToWide(const std::string& narrow) noexcept
    {
        try {
            if (narrow.empty() || narrow.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
                return {};
            }

            const int input_size = static_cast<int>(narrow.size());
            const int output_size = MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, narrow.data(), input_size, nullptr, 0);
            if (output_size <= 0) {
                return {};
            }

            std::wstring wide(static_cast<size_t>(output_size), L'\0');
            if (MultiByteToWideChar(
                    CP_UTF8, MB_ERR_INVALID_CHARS, narrow.data(), input_size,
                    wide.data(), output_size) != output_size) {
                return {};
            }
            return wide;
        }
        catch (...) {
            return {};
        }
    }

    std::string ToNarrow(const std::wstring& wide) noexcept
    {
        try {
            if (wide.empty() || wide.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
                return {};
            }

            const int input_size = static_cast<int>(wide.size());
            const int output_size = WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), input_size, nullptr, 0, nullptr, nullptr);
            if (output_size <= 0) {
                return {};
            }

            std::string narrow(static_cast<size_t>(output_size), '\0');
            if (WideCharToMultiByte(
                    CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), input_size,
                    narrow.data(), output_size, nullptr, nullptr) != output_size) {
                return {};
            }
            return narrow;
        }
        catch (...) {
            return {};
        }
    }
}

namespace pmon::util
{
    int64_t GetCurrentTimestamp() noexcept
    {
        LARGE_INTEGER timestamp{};
        return QueryPerformanceCounter(&timestamp) ? timestamp.QuadPart : 0;
    }

    double GetTimestampFrequencyDouble() noexcept
    {
        return static_cast<double>(GetTimestampFrequencyUint64());
    }

    uint64_t GetTimestampFrequencyUint64() noexcept
    {
        LARGE_INTEGER frequency{};
        return QueryPerformanceFrequency(&frequency)
            ? static_cast<uint64_t>(frequency.QuadPart)
            : 0;
    }

    double GetTimestampPeriodSeconds() noexcept
    {
        const double frequency = GetTimestampFrequencyDouble();
        return frequency > 0.0 ? 1.0 / frequency : 0.0;
    }

    void SpinWaitUntilTimestamp(int64_t timestamp) noexcept
    {
        while (GetCurrentTimestamp() < timestamp) {
            std::this_thread::yield();
        }
    }

    double TimestampDeltaToSeconds(int64_t start, int64_t end, double period) noexcept
    {
        return static_cast<double>(end - start) * period;
    }

    double TimestampDeltaToMilliSeconds(uint64_t duration, uint64_t qpc_frequency) noexcept
    {
        return qpc_frequency == 0
            ? 0.0
            : static_cast<double>(duration) * 1000.0 / static_cast<double>(qpc_frequency);
    }

    double TimestampDeltaToMilliSeconds(
        uint64_t start, uint64_t end, uint64_t qpc_frequency) noexcept
    {
        return end <= start
            ? 0.0
            : TimestampDeltaToMilliSeconds(end - start, qpc_frequency);
    }

    double TimestampDeltaToSignedMilliSeconds(
        uint64_t start, uint64_t end, uint64_t qpc_frequency) noexcept
    {
        if (qpc_frequency == 0 || start == 0 || end == 0 || start == end) {
            return 0.0;
        }
        return end > start
            ? TimestampDeltaToMilliSeconds(end - start, qpc_frequency)
            : -TimestampDeltaToMilliSeconds(start - end, qpc_frequency);
    }

    QpcTimer::QpcTimer(int64_t seeded_start_timestamp)
        : performanceCounterPeriod_(GetTimestampPeriodSeconds())
        , startTimestamp_(seeded_start_timestamp)
    {
    }

    QpcTimer::QpcTimer() noexcept
        : performanceCounterPeriod_(GetTimestampPeriodSeconds())
        , startTimestamp_(GetCurrentTimestamp())
    {
    }

    double QpcTimer::Mark() noexcept
    {
        const int64_t new_timestamp = GetCurrentTimestamp();
        const double delta = TimestampDeltaToSeconds(
            startTimestamp_, new_timestamp, performanceCounterPeriod_);
        startTimestamp_ = new_timestamp;
        return delta;
    }

    double QpcTimer::Peek() const noexcept
    {
        return TimestampDeltaToSeconds(
            startTimestamp_, GetCurrentTimestamp(), performanceCounterPeriod_);
    }

    int64_t QpcTimer::GetStartTimestamp() const noexcept
    {
        return startTimestamp_;
    }

    double QpcTimer::GetPerformanceCounterPeriod() const noexcept
    {
        return performanceCounterPeriod_;
    }

    double QpcTimer::SpinWaitUntil(double seconds) const noexcept
    {
        double elapsed = Peek();
        while (elapsed < seconds) {
            std::this_thread::yield();
            elapsed = Peek();
        }
        return elapsed - seconds;
    }

    int64_t QpcTimer::TimeToTimestamp(double seconds) const noexcept
    {
        return performanceCounterPeriod_ > 0.0
            ? startTimestamp_ + static_cast<int64_t>(seconds / performanceCounterPeriod_)
            : startTimestamp_;
    }

    double QpcConverter::TicksToMilliSeconds(uint64_t ticks) const noexcept
    {
        return static_cast<double>(ticks) * msPerTick_;
    }

    double QpcConverter::DeltaUnsignedMilliSeconds(uint64_t start, uint64_t end) const noexcept
    {
        return start == 0 || end <= start ? 0.0 : TicksToMilliSeconds(end - start);
    }

    double QpcConverter::DeltaSignedMilliSeconds(uint64_t start, uint64_t end) const noexcept
    {
        if (start == 0 || end == 0 || start == end) {
            return 0.0;
        }
        return end > start
            ? TicksToMilliSeconds(end - start)
            : -TicksToMilliSeconds(start - end);
    }

    double QpcConverter::DurationMilliSeconds(uint64_t tick_count) const noexcept
    {
        return TicksToMilliSeconds(tick_count);
    }
}

namespace pmon::util::win
{
    Handle::Handle() = default;

    Handle::Handle(HandleType handle)
        : handle_(handle)
    {
    }

    Handle::~Handle()
    {
        if (*this) {
            CloseHandle(std::exchange(handle_, nullptr));
        }
    }

    Handle::Handle(Handle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr))
    {
    }

    Handle& Handle::operator=(Handle&& other) noexcept
    {
        if (this != &other) {
            if (*this) {
                CloseHandle(handle_);
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    Handle::operator HandleType() const
    {
        return handle_;
    }

    Handle::HandleType Handle::Get() const
    {
        return handle_;
    }

    void Handle::Clear()
    {
        if (!*this) {
            handle_ = nullptr;
            return;
        }

        const HandleType handle = std::exchange(handle_, nullptr);
        if (!CloseHandle(handle)) {
            throw Except<Exception>("PresentMon compatibility layer failed to close a handle");
        }
    }

    Handle Handle::Clone() const
    {
        return *this ? CreateCloned(handle_) : Handle{};
    }

    Handle::HandleType Handle::Release()
    {
        return std::exchange(handle_, nullptr);
    }

    Handle::HandleType* Handle::ClearAndGetAddressOf()
    {
        Clear();
        return &handle_;
    }

    Handle::operator bool() const noexcept
    {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    Handle Handle::CreateCloned(HandleType handle)
    {
        HANDLE cloned_handle = nullptr;
        const HANDLE process = GetCurrentProcess();
        if (!DuplicateHandle(
                process, handle, process, &cloned_handle, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            throw Except<Exception>("PresentMon compatibility layer failed to clone a handle");
        }
        return Handle{ cloned_handle };
    }
}

namespace pmon::util
{
    PrecisionWaiter::PrecisionWaiter(double default_wait_buffer) noexcept
        : defaultWaitBuffer_(std::max(0.0, default_wait_buffer))
        , waitableTimer_(CreateWaitableTimerExW(
              nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS))
    {
        if (!waitableTimer_) {
            waitableTimer_ = win::Handle(CreateWaitableTimerW(nullptr, FALSE, nullptr));
        }
    }

    double PrecisionWaiter::Wait(double seconds, bool alertable) noexcept
    {
        return WaitWithBuffer(seconds, defaultWaitBuffer_, alertable);
    }

    double PrecisionWaiter::WaitUnbuffered(double seconds, bool alertable) noexcept
    {
        return WaitWithBuffer(seconds, 0.0, alertable);
    }

    double PrecisionWaiter::WaitWithBuffer(double seconds, double buffer, bool alertable) noexcept
    {
        if (seconds <= 0.0) {
            return 0.0;
        }

        const double safe_buffer = std::clamp(buffer, 0.0, seconds);
        const double timer_seconds = seconds - safe_buffer;
        if (safe_buffer > 0.0) {
            qpcTimer_.Mark();
        }

        if (waitableTimer_ && timer_seconds > 0.0) {
            LARGE_INTEGER due_time{};
            due_time.QuadPart = -static_cast<LONGLONG>(timer_seconds * 10'000'000.0);
            if (SetWaitableTimerEx(
                    waitableTimer_, &due_time, 0, nullptr, nullptr, nullptr, 0)) {
                DWORD wait_result = WAIT_IO_COMPLETION;
                while (wait_result == WAIT_IO_COMPLETION) {
                    wait_result = WaitForSingleObjectEx(waitableTimer_, INFINITE, alertable);
                }
                if (wait_result == WAIT_OBJECT_0) {
                    return safe_buffer > 0.0 ? qpcTimer_.SpinWaitUntil(seconds) : 0.0;
                }
            }
        }

        Sleep(static_cast<DWORD>(std::max(0.0, timer_seconds) * 1000.0));
        return safe_buffer > 0.0 ? qpcTimer_.SpinWaitUntil(seconds) : 0.0;
    }

    Exception::Exception(std::string message) noexcept
        : note_(std::move(message))
    {
    }

    void Exception::CaptureStackTrace()
    {
        // Stack tracing belongs to PresentMon's full logging runtime and is
        // intentionally unavailable in this narrow compatibility layer.
    }

    const char* Exception::what() const noexcept
    {
        if (buffer_.empty()) {
            buffer_ = ComposeWhatString_();
        }
        return buffer_.c_str();
    }

    const std::string& Exception::GetNote() const
    {
        return note_;
    }

    std::string Exception::GetTraceString() const
    {
        return {};
    }

    bool Exception::HasTrace() const noexcept
    {
        return false;
    }

    PM_STATUS Exception::GeneratePmStatus() const noexcept
    {
        return PM_STATUS_FAILURE;
    }

    bool Exception::HasPmStatus() const noexcept
    {
        return false;
    }

    std::string Exception::ComposeWhatString_() const noexcept
    {
        try {
            return note_;
        }
        catch (...) {
            return {};
        }
    }

    void DoCapture_(Exception&)
    {
    }

    std::pair<std::string, std::optional<PM_STATUS>> ReportException(
        std::string note, std::exception_ptr exception) noexcept
    {
        try {
            if (!exception) {
                exception = std::current_exception();
            }
            if (!exception) {
                return { note.empty() ? "No exception in flight" : std::move(note), std::nullopt };
            }

            try {
                std::rethrow_exception(exception);
            }
            catch (const Exception& caught) {
                std::string message = caught.what();
                if (!note.empty()) {
                    message = std::move(note) + "\n" + message;
                }
                return {
                    std::move(message),
                    caught.HasPmStatus()
                        ? std::optional<PM_STATUS>{ caught.GeneratePmStatus() }
                        : std::nullopt
                };
            }
            catch (const std::exception& caught) {
                std::string message = caught.what();
                if (!note.empty()) {
                    message = std::move(note) + "\n" + message;
                }
                return { std::move(message), std::nullopt };
            }
            catch (...) {
                return {
                    note.empty() ? "Unrecognized exception" : std::move(note) + "\nUnrecognized exception",
                    std::nullopt
                };
            }
        }
        catch (...) {
            return {};
        }
    }

    PM_STATUS GeneratePmStatus(std::exception_ptr exception) noexcept
    {
        if (!exception) {
            exception = std::current_exception();
        }
        if (!exception) {
            return PM_STATUS_SUCCESS;
        }

        try {
            std::rethrow_exception(exception);
        }
        catch (const Exception& caught) {
            return caught.GeneratePmStatus();
        }
        catch (...) {
            return PM_STATUS_FAILURE;
        }
    }

    SehException::SehException(unsigned int code) noexcept
        : sehCode(code)
    {
    }

    unsigned int SehException::GetSehCode() const noexcept
    {
        return sehCode;
    }

    std::string SehException::ComposeWhatString_() const noexcept
    {
        char buffer[64]{};
        std::snprintf(buffer, sizeof(buffer), "Structured exception 0x%08X", sehCode);
        return buffer;
    }

    void InstallSehTranslator() noexcept
    {
        // The PresentData subset used by ReLimiter does not execute through
        // CommonUtilities' exception-translation boundary. Keep this ABI
        // entry point inert instead of requiring /EHa for the entire addon.
    }
}

namespace pmon::util::log
{
    GlobalPolicy::GlobalPolicy() noexcept
        : logLevel_(Level::None)
        , traceLevel_(Level::None)
    {
    }

    GlobalPolicy& GlobalPolicy::Get() noexcept
    {
        static GlobalPolicy policy;
        return policy;
    }

    Level GlobalPolicy::GetLogLevel() const noexcept
    {
        return logLevel_.load(std::memory_order_relaxed);
    }

    void GlobalPolicy::SetLogLevel(Level level) noexcept
    {
        logLevel_.store(level, std::memory_order_relaxed);
    }

    void GlobalPolicy::SetLogLevelDefault() noexcept
    {
        SetLogLevel(Level::None);
    }

    Level GlobalPolicy::GetTraceLevel() const noexcept
    {
        return traceLevel_.load(std::memory_order_relaxed);
    }

    void GlobalPolicy::SetTraceLevel(Level level) noexcept
    {
        traceLevel_.store(level, std::memory_order_relaxed);
    }

    void GlobalPolicy::SetTraceLevelDefault() noexcept
    {
        SetTraceLevel(Level::None);
    }

    bool GlobalPolicy::GetResolveTraceInClientThread() const noexcept
    {
        return resolveTraceInClientThread_.load(std::memory_order_relaxed);
    }

    void GlobalPolicy::SetResolveTraceInClientThread(bool setting) noexcept
    {
        resolveTraceInClientThread_.store(setting, std::memory_order_relaxed);
    }

    bool GlobalPolicy::GetExceptionTrace() const noexcept
    {
        return exceptionTracePolicy_.load(std::memory_order_relaxed);
    }

    void GlobalPolicy::SetExceptionTrace(bool policy) noexcept
    {
        exceptionTracePolicy_.store(policy, std::memory_order_relaxed);
    }

    bool GlobalPolicy::GetSehTracing() const noexcept
    {
        return sehTraceOn_.load(std::memory_order_relaxed);
    }

    void GlobalPolicy::SetSehTracing(bool enabled) noexcept
    {
        sehTraceOn_.store(enabled, std::memory_order_relaxed);
    }

    Subsystem GlobalPolicy::GetSubsystem() const noexcept
    {
        return subsystem_.load(std::memory_order_relaxed);
    }

    void GlobalPolicy::SetSubsystem(Subsystem subsystem) noexcept
    {
        subsystem_.store(subsystem, std::memory_order_relaxed);
    }

    void GlobalPolicy::ActivateVerboseModule(V module) noexcept
    {
        activeVerboseModules_.fetch_or(
            1ull << static_cast<uint64_t>(module), std::memory_order_relaxed);
    }

    void GlobalPolicy::DeactivateVerboseModule(V module) noexcept
    {
        activeVerboseModules_.fetch_and(
            ~(1ull << static_cast<uint64_t>(module)), std::memory_order_relaxed);
    }

    bool GlobalPolicy::CheckVerboseModule(V module) const noexcept
    {
        return (activeVerboseModules_.load(std::memory_order_relaxed) &
            (1ull << static_cast<uint64_t>(module))) != 0;
    }

    void GlobalPolicy::ClearVerboseModules() noexcept
    {
        activeVerboseModules_.store(0, std::memory_order_relaxed);
    }

    void GlobalPolicy::StoreVerboseModules(uint64_t modules) noexcept
    {
        activeVerboseModules_.store(modules, std::memory_order_relaxed);
    }
}

namespace pmon::util::log
{
    ErrorCode::ErrorCode(const ErrorCode& other)
        : pTypeInfo_(other.pTypeInfo_)
        , pStrings_(other.pStrings_)
        , integral_(other.integral_)
    {
    }

    ErrorCode::ErrorCode(ErrorCode&& other)
        : pTypeInfo_(std::exchange(other.pTypeInfo_, nullptr))
        , pStrings_(std::move(other.pStrings_))
        , integral_(std::move(other.integral_))
    {
        other.integral_ = std::monostate{};
    }

    ErrorCode& ErrorCode::operator=(ErrorCode&& other)
    {
        if (this != &other) {
            pTypeInfo_ = std::exchange(other.pTypeInfo_, nullptr);
            pStrings_ = std::move(other.pStrings_);
            integral_ = std::move(other.integral_);
            other.integral_ = std::monostate{};
        }
        return *this;
    }

    bool ErrorCode::HasUnsigned() const
    {
        return std::holds_alternative<uint64_t>(integral_);
    }

    bool ErrorCode::HasSigned() const
    {
        return std::holds_alternative<int64_t>(integral_);
    }

    bool ErrorCode::HasIntegral() const
    {
        return HasUnsigned() || HasSigned();
    }

    bool ErrorCode::Fits32() const
    {
        if (const auto value = AsSigned()) {
            return *value >= std::numeric_limits<int32_t>::min() &&
                *value <= std::numeric_limits<int32_t>::max();
        }
        if (const auto value = AsUnsigned()) {
            return *value <= std::numeric_limits<uint32_t>::max();
        }
        return false;
    }

    std::optional<uint64_t> ErrorCode::AsUnsigned() const
    {
        if (const auto* value = std::get_if<uint64_t>(&integral_)) {
            return *value;
        }
        if (const auto* value = std::get_if<int64_t>(&integral_); value && *value >= 0) {
            return static_cast<uint64_t>(*value);
        }
        return std::nullopt;
    }

    std::optional<int64_t> ErrorCode::AsSigned() const
    {
        if (const auto* value = std::get_if<int64_t>(&integral_)) {
            return *value;
        }
        if (const auto* value = std::get_if<uint64_t>(&integral_);
            value && *value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return static_cast<int64_t>(*value);
        }
        return std::nullopt;
    }

    bool ErrorCode::Empty() const
    {
        return !HasIntegral();
    }

    bool ErrorCode::Resolve(const IErrorCodeResolver& resolver)
    {
        if (pTypeInfo_ == nullptr || Empty()) {
            return false;
        }
        pStrings_ = CloningUptr<IErrorCodeResolver::Strings>(
            new IErrorCodeResolver::Strings(resolver.Resolve(std::type_index(*pTypeInfo_), *this)));
        return true;
    }

    bool ErrorCode::HasTypeInfo() const
    {
        return pTypeInfo_ != nullptr;
    }

    bool ErrorCode::IsResolved() const
    {
        return static_cast<bool>(pStrings_);
    }

    bool ErrorCode::IsResolvedNontrivial() const
    {
        return pStrings_ &&
            (!pStrings_->symbol.empty() || !pStrings_->name.empty() ||
             !pStrings_->description.empty());
    }

    std::string ErrorCode::AsHex() const
    {
        char buffer[32]{};
        if (const auto value = AsUnsigned()) {
            std::snprintf(
                buffer, sizeof(buffer), "0x%llX",
                static_cast<unsigned long long>(*value));
            return buffer;
        }
        if (const auto value = AsSigned()) {
            std::snprintf(
                buffer, sizeof(buffer), "0x%llX",
                static_cast<unsigned long long>(*value));
            return buffer;
        }
        return {};
    }

    const IErrorCodeResolver::Strings* ErrorCode::GetStrings() const
    {
        return pStrings_.get();
    }

    ErrorCode::operator bool() const
    {
        return !Empty();
    }

    EntryBuilder::EntryBuilder(
        Level level,
        const char* source_file,
        const char* source_function,
        int source_line) noexcept
        : Entry{}
        , traceSkipDepth_(0)
    {
        level_ = level;
        sourceStrings_ = Entry::StaticSourceStrings{ source_file, source_function };
        sourceLine_ = source_line;
        committed_ = true;
    }

    EntryBuilder::EntryBuilder(
        Level level,
        std::string source_file,
        std::string source_function,
        int source_line) noexcept
        : Entry{}
        , traceSkipDepth_(0)
    {
        level_ = level;
        sourceStrings_ = Entry::HeapedSourceStrings{
            std::move(source_file), std::move(source_function) };
        sourceLine_ = source_line;
        committed_ = true;
    }

    EntryBuilder::~EntryBuilder() = default;

    EntryBuilder& EntryBuilder::note(std::string note) noexcept
    {
        note_ = std::move(note);
        return *this;
    }

    EntryBuilder& EntryBuilder::to(std::shared_ptr<IEntrySink>) noexcept
    {
        return *this;
    }

    EntryBuilder& EntryBuilder::hr() noexcept
    {
        return hr(GetLastError());
    }

    EntryBuilder& EntryBuilder::hr(uint32_t error) noexcept
    {
        errorCode_ = ErrorCode{ error };
        return *this;
    }

    EntryBuilder& EntryBuilder::subsys(Subsystem subsystem) noexcept
    {
        subsystem_ = subsystem;
        return *this;
    }

    std::shared_ptr<IChannel> GetDefaultChannel() noexcept
    {
        return {};
    }
}
