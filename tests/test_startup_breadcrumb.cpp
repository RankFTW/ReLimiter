// Direct production tests of the start-up breadcrumb (src/startup_breadcrumb.cpp): the judgement of a previous
// launch's record, the report text, the marks (first-wins, nothing after Running except Closed, the 32nd completed
// present marks Running) and the attach protocol on real temporary files. Dropping the mapping without Closed is the
// kill equivalent: the record stays in the file and the next attach reports it. The logger's warning line and the
// hook sites need live validation.
#include "startup_breadcrumb.h"

#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)

namespace {

std::string TempPath(const char* name) {
    char dir[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    return std::string(dir) + "relimiter_crumb_test_" + std::to_string(GetCurrentProcessId()) + "_" + name + ".crumb";
}

void Remove(const std::string& path) {
    SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileA(path.c_str());
}

bool Exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Reads the record the way an analyst's tool does: a plain read open that shares with the writer.
bool ReadRecord(const std::string& path, BreadcrumbRecord* record) {
    const HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0;
    const bool ok = ReadFile(file, record, sizeof(*record), &got, nullptr) && got == sizeof(*record);
    CloseHandle(file);
    return ok;
}

void WriteBytes(const std::string& path, const void* data, size_t size) {
    const HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(file != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    CHECK(WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) && written == size);
    CloseHandle(file);
}

BreadcrumbRecord Good(StartupStage stage, uint8_t site = 0) {
    BreadcrumbRecord record{};
    record.magic = kBreadcrumbMagic;
    record.version = kBreadcrumbVersion;
    record.size = sizeof(BreadcrumbRecord);
    record.seq_begin = record.seq_end = 6;
    record.process_id = 1234;
    record.launch_filetime = 132000000000000000ull;
    record.stage_qpc = 5000;
    record.stage = static_cast<uint8_t>(stage);
    record.site = site;
    return record;
}

bool StageIs(const std::string& path, StartupStage stage, uint8_t site = 0) {
    BreadcrumbRecord record;
    return ReadRecord(path, &record) && record.stage == static_cast<uint8_t>(stage) && record.site == site &&
           record.seq_begin == record.seq_end && (record.seq_begin & 1u) == 0;
}

void TestJudge() {
    BreadcrumbVerdict verdict = Breadcrumb_Judge(Good(StartupStage::InitEnded));
    CHECK(verdict.report && verdict.stage == StartupStage::InitEnded && verdict.site == 0);
    verdict = Breadcrumb_Judge(Good(StartupStage::BatchApplyEntered, 3));
    CHECK(verdict.report && verdict.stage == StartupStage::BatchApplyEntered && verdict.site == 3);
    // Every stage before Running is reported, including None (the log opened, start-up never began).
    for (uint8_t stage = 0; stage < static_cast<uint8_t>(StartupStage::Running); ++stage) {
        BreadcrumbRecord record = Good(static_cast<StartupStage>(stage));
        if (stage == 0) record.stage_qpc = 0;
        CHECK(Breadcrumb_Judge(record).report);
    }
    CHECK(!Breadcrumb_Judge(Good(StartupStage::Running)).report);
    CHECK(!Breadcrumb_Judge(Good(StartupStage::Closed)).report);

    BreadcrumbRecord record = Good(StartupStage::InitEnded);
    record.magic ^= 1; CHECK(!Breadcrumb_Judge(record).report);
    record = Good(StartupStage::InitEnded); record.version = kBreadcrumbVersion + 1; CHECK(!Breadcrumb_Judge(record).report);
    record = Good(StartupStage::InitEnded); record.size = 8; CHECK(!Breadcrumb_Judge(record).report);
    record = Good(StartupStage::InitEnded); record.seq_begin = 7; record.seq_end = 7; CHECK(!Breadcrumb_Judge(record).report); // odd: mid-write
    record = Good(StartupStage::InitEnded); record.seq_begin = 7; record.seq_end = 6; CHECK(!Breadcrumb_Judge(record).report);
    record = Good(StartupStage::InitEnded); record.seq_end = 8; CHECK(!Breadcrumb_Judge(record).report);               // mismatched
    record = Good(StartupStage::InitEnded); record.process_id = 0; CHECK(!Breadcrumb_Judge(record).report);
    record = Good(StartupStage::InitEnded); record.launch_filetime = 0; CHECK(!Breadcrumb_Judge(record).report);
    record = Good(StartupStage::InitEnded); record.stage = kStartupStageLast + 1; CHECK(!Breadcrumb_Judge(record).report);
    record = Good(StartupStage::InitEnded); record.stage_qpc = 0; CHECK(!Breadcrumb_Judge(record).report);
}

void TestDescribe() {
    char text[256];
    BreadcrumbVerdict verdict;
    CHECK(Breadcrumb_Describe(verdict, text, sizeof(text)) == 0 && text[0] == '\0');

    verdict.report = true;
    verdict.stage = StartupStage::FirstGateSignal;
    CHECK(Breadcrumb_Describe(verdict, text, sizeof(text)) > 0);
    CHECK(std::strcmp(text, "the previous launch ended during start-up; last stage: FirstGateSignal") == 0);

    verdict.stage = StartupStage::BatchApplyEntered;
    verdict.site = 3;
    Breadcrumb_Describe(verdict, text, sizeof(text));
    CHECK(std::strcmp(text, "the previous launch ended during start-up; last stage: BatchApplyEntered (site LoaderScan)") == 0);
    verdict.stage = StartupStage::BatchApplyLeft;
    verdict.site = 5;
    Breadcrumb_Describe(verdict, text, sizeof(text));
    CHECK(std::strstr(text, "BatchApplyLeft (site DxgiFallback)") != nullptr);
    verdict.site = 30;
    Breadcrumb_Describe(verdict, text, sizeof(text));
    CHECK(std::strstr(text, "BatchApplyLeft (site 30)") != nullptr);

    // The installer flags in the site byte: the site stays readable, the flags are named for the batch stages.
    verdict.site = 3;
    verdict.flags = kBreadcrumbInstallerRan | kBreadcrumbInstallerProtected;
    Breadcrumb_Describe(verdict, text, sizeof(text));
    CHECK(std::strstr(text, "BatchApplyLeft (site LoaderScan); the apply ran on the installer thread") != nullptr);
    verdict.flags = kBreadcrumbInstallerProtected;
    Breadcrumb_Describe(verdict, text, sizeof(text));
    CHECK(std::strstr(text, "(site LoaderScan); installer thread protected, the apply ran on the requester") != nullptr);
    verdict.flags = kBreadcrumbInstallerUnprotected;
    Breadcrumb_Describe(verdict, text, sizeof(text));
    CHECK(std::strstr(text, "(site LoaderScan); installer thread unprotected, the apply ran on the requester") != nullptr);
    verdict.stage = StartupStage::FirstGateSignal;  // other stages carry no installer text
    verdict.site = 0;
    Breadcrumb_Describe(verdict, text, sizeof(text));
    CHECK(std::strcmp(text, "the previous launch ended during start-up; last stage: FirstGateSignal") == 0);
    verdict.stage = StartupStage::BatchApplyLeft;
    verdict.site = 5;
    verdict.flags = 0;
    {
        const BreadcrumbVerdict judged = Breadcrumb_Judge(Good(StartupStage::BatchApplyEntered,
            static_cast<uint8_t>(3 | kBreadcrumbInstallerRan | kBreadcrumbInstallerProtected)));
        CHECK(judged.report && judged.site == 3 && judged.flags == (kBreadcrumbInstallerRan | kBreadcrumbInstallerProtected));
    }

    char small[16];
    const int n = Breadcrumb_Describe(verdict, small, sizeof(small));
    CHECK(n == static_cast<int>(sizeof(small) - 1) && std::strlen(small) == sizeof(small) - 1);
}

void TestWriteKillRead() {
    const std::string path = TempPath("kill");
    Remove(path);
    // A missing file: nothing to report; the file is created and holds this launch's record at stage None.
    CHECK(!Breadcrumb_Attach(path.c_str()).report);
    BreadcrumbRecord record;
    CHECK(ReadRecord(path, &record));
    CHECK(record.magic == kBreadcrumbMagic && record.version == kBreadcrumbVersion && record.size == sizeof(record));
    CHECK(record.process_id == GetCurrentProcessId() && record.launch_filetime != 0);
    CHECK(StageIs(path, StartupStage::None));

    Breadcrumb_Mark(StartupStage::InitBegun);
    CHECK(StageIs(path, StartupStage::InitBegun));
    Breadcrumb_Mark(StartupStage::InitEnded);
    Breadcrumb_Mark(StartupStage::BatchApplyEntered, 2);
    CHECK(StageIs(path, StartupStage::BatchApplyEntered, 2));
    CHECK(ReadRecord(path, &record) && record.stage_qpc != 0);

    // The process dies here: the mapping goes away without Closed.
    Breadcrumb_Detach();
    CHECK(StageIs(path, StartupStage::BatchApplyEntered, 2));
    const BreadcrumbVerdict verdict = Breadcrumb_Attach(path.c_str());
    CHECK(verdict.report && verdict.stage == StartupStage::BatchApplyEntered && verdict.site == 2);
    CHECK(StageIs(path, StartupStage::None)); // the file was reset for the new launch
    Breadcrumb_Detach();
    Remove(path);
}

void TestFirstWinsAndBatches() {
    const std::string path = TempPath("firstwins");
    Remove(path);
    CHECK(!Breadcrumb_Attach(path.c_str()).report);
    Breadcrumb_Mark(StartupStage::None);                  // not a stage
    Breadcrumb_Mark(static_cast<StartupStage>(200));      // not a stage
    CHECK(StageIs(path, StartupStage::None));
    Breadcrumb_Mark(StartupStage::FirstPresentEvent, 1);
    Breadcrumb_Mark(StartupStage::InitBegun);
    Breadcrumb_Mark(StartupStage::InitBegun, 9);          // first-wins: ignored
    CHECK(StageIs(path, StartupStage::InitBegun));
    Breadcrumb_Mark(StartupStage::FirstPresentEvent, 2);  // first-wins: ignored
    CHECK(StageIs(path, StartupStage::InitBegun));
    Breadcrumb_Mark(StartupStage::BatchApplyEntered, 1);
    Breadcrumb_Mark(StartupStage::BatchApplyLeft, 1);
    Breadcrumb_Mark(StartupStage::BatchApplyEntered, 3);  // batches repeat
    CHECK(StageIs(path, StartupStage::BatchApplyEntered, 3));
    Breadcrumb_Detach();
    Remove(path);
}

void TestRunningAndClosed() {
    const std::string path = TempPath("running");
    Remove(path);

    // 32 completed presents with the first-present event marked: Running; a later batch is not start-up.
    CHECK(!Breadcrumb_Attach(path.c_str()).report);
    Breadcrumb_Mark(StartupStage::InitEnded);
    Breadcrumb_Mark(StartupStage::FirstPresentEvent);
    for (int i = 0; i < 31; ++i) Breadcrumb_PresentCompleted();
    CHECK(StageIs(path, StartupStage::FirstPresentEvent));
    Breadcrumb_PresentCompleted();
    CHECK(StageIs(path, StartupStage::Running));
    Breadcrumb_Mark(StartupStage::BatchApplyEntered, 3);
    Breadcrumb_Mark(StartupStage::FirstGateSignal);
    for (int i = 0; i < 100; ++i) Breadcrumb_PresentCompleted();
    CHECK(StageIs(path, StartupStage::Running));
    Breadcrumb_Detach();                                  // killed while running
    CHECK(!Breadcrumb_Attach(path.c_str()).report);       // reached Running: nothing is reported
    Breadcrumb_Detach();

    // Presents without the first-present event never mark Running. (The record the previous attach left is stage None.)
    CHECK(Breadcrumb_Attach(path.c_str()).report);
    Breadcrumb_Mark(StartupStage::InitBegun);
    for (int i = 0; i < 100; ++i) Breadcrumb_PresentCompleted();
    CHECK(StageIs(path, StartupStage::InitBegun));
    Breadcrumb_Detach();
    CHECK(Breadcrumb_Attach(path.c_str()).report);
    Breadcrumb_Detach();

    // Closed ends the record; Running after it and Closed after Running both behave.
    CHECK(Breadcrumb_Attach(path.c_str()).report);        // the stage None left by the previous attach
    Breadcrumb_Mark(StartupStage::InitBegun);
    Breadcrumb_Mark(StartupStage::Closed);
    Breadcrumb_Mark(StartupStage::Running);
    Breadcrumb_Mark(StartupStage::BatchApplyEntered, 1);
    CHECK(StageIs(path, StartupStage::Closed));
    Breadcrumb_Detach();
    CHECK(!Breadcrumb_Attach(path.c_str()).report);

    Breadcrumb_Mark(StartupStage::FirstPresentEvent);
    for (int i = 0; i < 32; ++i) Breadcrumb_PresentCompleted();
    Breadcrumb_Mark(StartupStage::Closed);
    CHECK(StageIs(path, StartupStage::Closed));
    Breadcrumb_Detach();
    CHECK(!Breadcrumb_Attach(path.c_str()).report);
    Breadcrumb_Detach();
    Remove(path);
}

void TestReloadInProcess() {
    const std::string path = TempPath("reload");
    Remove(path);
    CHECK(!Breadcrumb_Attach(path.c_str()).report);
    Breadcrumb_Mark(StartupStage::InitBegun);
    Breadcrumb_Mark(StartupStage::Closed);
    // ReShade reloads the addon: the logger attaches again with the mapping still in place.
    CHECK(!Breadcrumb_Attach(path.c_str()).report);       // the previous session closed cleanly
    CHECK(StageIs(path, StartupStage::None));
    Breadcrumb_Mark(StartupStage::InitBegun);             // the new session marks again (first-wins state was reset)
    CHECK(StageIs(path, StartupStage::InitBegun));
    const BreadcrumbVerdict verdict = Breadcrumb_Attach(path.c_str());
    CHECK(verdict.report && verdict.stage == StartupStage::InitBegun);
    Breadcrumb_Detach();
    Remove(path);
}

void TestUnusableRecords() {
    const std::string path = TempPath("torn");
    Remove(path);

    // The harness itself: a good record before Running is reported through the file.
    BreadcrumbRecord record = Good(StartupStage::InitEnded);
    WriteBytes(path, &record, sizeof(record));
    BreadcrumbVerdict verdict = Breadcrumb_Attach(path.c_str());
    CHECK(verdict.report && verdict.stage == StartupStage::InitEnded);
    Breadcrumb_Detach();

    auto expect_silent = [&](const BreadcrumbRecord& bad) {
        WriteBytes(path, &bad, sizeof(bad));
        CHECK(!Breadcrumb_Attach(path.c_str()).report);
        CHECK(StageIs(path, StartupStage::None));         // still reset, and marks work
        Breadcrumb_Mark(StartupStage::InitBegun);
        CHECK(StageIs(path, StartupStage::InitBegun));
        Breadcrumb_Detach();
    };
    record = Good(StartupStage::InitEnded); record.seq_begin = 7; expect_silent(record);   // torn: odd seq
    record = Good(StartupStage::InitEnded); record.seq_end = 8; expect_silent(record);     // torn: mismatched seq
    record = Good(StartupStage::InitEnded); record.magic = 0; expect_silent(record);       // bad magic
    record = Good(StartupStage::InitEnded); record.version = 99; expect_silent(record);    // unknown version

    // Short file, empty file, garbage.
    WriteBytes(path, &record, 12);
    CHECK(!Breadcrumb_Attach(path.c_str()).report);
    Breadcrumb_Detach();
    WriteBytes(path, "", 0);
    CHECK(!Breadcrumb_Attach(path.c_str()).report);
    Breadcrumb_Detach();
    std::vector<unsigned char> garbage(kBreadcrumbFileSize, 0xAB);
    WriteBytes(path, garbage.data(), garbage.size());
    CHECK(!Breadcrumb_Attach(path.c_str()).report);
    Breadcrumb_Detach();
    Remove(path);
}

void TestUnusablePaths() {
    // A folder that does not exist: no file, no report, the marks do nothing and nothing crashes.
    const std::string missing_dir = TempPath("nodir") + "\\x\\y.crumb";
    CHECK(!Breadcrumb_Attach(missing_dir.c_str()).report);
    CHECK(!Breadcrumb_Attach(nullptr).report);
    CHECK(!Breadcrumb_Attach("").report);
    Breadcrumb_Mark(StartupStage::InitBegun);
    Breadcrumb_Mark(StartupStage::Closed);
    Breadcrumb_PresentCompleted();
    CHECK(!Exists(missing_dir));

    // A read-only file: the writable open fails; the record on disk is left as it was and the marks do nothing.
    const std::string readonly = TempPath("readonly");
    Remove(readonly);
    BreadcrumbRecord record = Good(StartupStage::InitBegun);
    WriteBytes(readonly, &record, sizeof(record));
    CHECK(SetFileAttributesA(readonly.c_str(), FILE_ATTRIBUTE_READONLY));
    CHECK(!Breadcrumb_Attach(readonly.c_str()).report);
    Breadcrumb_Mark(StartupStage::InitEnded);
    Breadcrumb_Mark(StartupStage::Closed);
    CHECK(StageIs(readonly, StartupStage::InitBegun));
    Remove(readonly);

    // A file another live launch holds (exclusive writer): a sharing violation, no report, the record untouched.
    const std::string held = TempPath("held");
    Remove(held);
    const HANDLE other = CreateFileA(held.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(other != INVALID_HANDLE_VALUE);
    record = Good(StartupStage::InitEnded);
    DWORD written = 0;
    CHECK(WriteFile(other, &record, sizeof(record), &written, nullptr));
    CHECK(!Breadcrumb_Attach(held.c_str()).report);
    Breadcrumb_Mark(StartupStage::Closed);
    CHECK(StageIs(held, StartupStage::InitEnded));
    CloseHandle(other);
    Remove(held);
}

void TestConcurrentMarks() {
    const std::string path = TempPath("threads");
    Remove(path);
    CHECK(!Breadcrumb_Attach(path.c_str()).report);
    // Writers contend for the record: a mark that finds another writing is dropped, never blocked or torn.
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < 20000; ++i) {
                Breadcrumb_Mark(StartupStage::BatchApplyEntered, static_cast<uint8_t>(1 + t));
                Breadcrumb_Mark(StartupStage::BatchApplyLeft, static_cast<uint8_t>(1 + t));
            }
        });
    }
    for (int i = 0; i < 2000; ++i) {
        BreadcrumbRecord record;
        CHECK(ReadRecord(path, &record));
        // Whenever a reader sees a consistent record it is a whole one.
        if (record.seq_begin == record.seq_end && (record.seq_begin & 1u) == 0) {
            CHECK(record.stage == static_cast<uint8_t>(StartupStage::None) ||
                  record.stage == static_cast<uint8_t>(StartupStage::BatchApplyEntered) ||
                  record.stage == static_cast<uint8_t>(StartupStage::BatchApplyLeft));
            CHECK(record.stage == 0 || (record.site >= 1 && record.site <= 4));
        }
    }
    for (auto& thread : threads) thread.join();
    BreadcrumbRecord record;
    CHECK(ReadRecord(path, &record));
    CHECK(record.seq_begin == record.seq_end && (record.seq_begin & 1u) == 0);
    CHECK(Breadcrumb_Judge(record).report);
    Breadcrumb_Detach();
    Remove(path);
}

} // namespace

int main() {
    TestJudge();
    TestDescribe();
    TestWriteKillRead();
    TestFirstWinsAndBatches();
    TestRunningAndClosed();
    TestReloadInProcess();
    TestUnusableRecords();
    TestUnusablePaths();
    TestConcurrentMarks();
    std::puts("test_startup_breadcrumb: all tests passed");
    return 0;
}
