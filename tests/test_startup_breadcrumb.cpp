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

// The trail (the ring in the crumb page): every mark appends once (first-wins stages once, batch stages each time),
// also after Running, in order with the thread and time, and the single record at offset 0 is not touched by it.
bool ReadPage(const std::string& path, std::vector<unsigned char>* page) {
    const HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    page->assign(kBreadcrumbFileSize, 0);
    DWORD got = 0;
    const bool ok = ReadFile(file, page->data(), static_cast<DWORD>(page->size()), &got, nullptr) && got == page->size();
    CloseHandle(file);
    return ok;
}

void TestTrail() {
    const std::string path = TempPath("trail");
    Remove(path);
    CHECK(!Breadcrumb_Attach(path.c_str()).report);
    TrailEntry entries[kTrailCapacity];
    CHECK(Breadcrumb_TrailSnapshot(entries, kTrailCapacity) == 0);  // attach alone is not a mark

    Breadcrumb_Mark(StartupStage::InitBegun);
    Breadcrumb_Mark(StartupStage::InitBegun);  // first-wins: not a second entry
    Breadcrumb_Mark(StartupStage::BatchApplyEntered, 0x83);
    Breadcrumb_Mark(StartupStage::BatchApplyLeft, 0x83);
    Breadcrumb_Mark(StartupStage::BatchApplyEntered, 0x83);  // batch stages are recorded every time
    size_t count = Breadcrumb_TrailSnapshot(entries, kTrailCapacity);
    CHECK(count == 4);
    CHECK(entries[0].kind == static_cast<uint8_t>(TrailKind::Stage) &&
          entries[0].site == static_cast<uint8_t>(StartupStage::InitBegun));
    CHECK(entries[1].site == static_cast<uint8_t>(StartupStage::BatchApplyEntered) && entries[1].aux == 0x83);
    CHECK(entries[2].site == static_cast<uint8_t>(StartupStage::BatchApplyLeft));
    CHECK(entries[3].site == static_cast<uint8_t>(StartupStage::BatchApplyEntered));
    for (size_t i = 0; i < count; ++i) {
        CHECK(entries[i].thread_id == GetCurrentThreadId());
        CHECK(entries[i].qpc != 0);
        CHECK(i == 0 || entries[i].qpc >= entries[i - 1].qpc);
    }
    // The single record is the last stored mark and is valid.
    CHECK(StageIs(path, StartupStage::BatchApplyEntered, 0x83));

    // A trail-only kind does not touch the record.
    Breadcrumb_Trail(TrailKind::FreezeEnter, 4, 0, 1);
    CHECK(StageIs(path, StartupStage::BatchApplyEntered, 0x83));
    count = Breadcrumb_TrailSnapshot(entries, kTrailCapacity);
    CHECK(count == 5 && entries[4].kind == static_cast<uint8_t>(TrailKind::FreezeEnter) && entries[4].site == 4 &&
          entries[4].aux == 1);
    // Kind 0 and a kind past the last are refused, nothing is appended.
    Breadcrumb_Trail(TrailKind::None);
    Breadcrumb_Trail(static_cast<TrailKind>(kTrailKindLast + 1));
    CHECK(Breadcrumb_TrailSnapshot(entries, kTrailCapacity) == 5);

    // After Running the record stops (only Closed follows) and the trail keeps recording, a first-wins stage once.
    Breadcrumb_Mark(StartupStage::FirstPresentEvent);
    for (int i = 0; i < 40; ++i) Breadcrumb_PresentCompleted();
    CHECK(StageIs(path, StartupStage::Running));
    const size_t before = Breadcrumb_TrailSnapshot(entries, kTrailCapacity);
    Breadcrumb_Mark(StartupStage::FirstGateSignal);
    Breadcrumb_Mark(StartupStage::FirstGateSignal);
    Breadcrumb_Mark(StartupStage::BatchApplyEntered, 0x03);
    CHECK(StageIs(path, StartupStage::Running));
    count = Breadcrumb_TrailSnapshot(entries, kTrailCapacity);
    CHECK(count == before + 2);
    CHECK(entries[count - 2].site == static_cast<uint8_t>(StartupStage::FirstGateSignal));
    CHECK(entries[count - 1].site == static_cast<uint8_t>(StartupStage::BatchApplyEntered) && entries[count - 1].aux == 0x03);
    bool running_in_trail = false;
    for (size_t i = 0; i < count; ++i) running_in_trail = running_in_trail || entries[i].site == static_cast<uint8_t>(StartupStage::Running);
    CHECK(running_in_trail);
    Breadcrumb_Mark(StartupStage::Closed);
    CHECK(StageIs(path, StartupStage::Closed));
    CHECK(Breadcrumb_TrailSnapshot(entries, kTrailCapacity) == count + 1);

    // The same entries from a copy of the file (what an analyst's tool reads back after a kill).
    std::vector<unsigned char> page;
    CHECK(ReadPage(path, &page));
    TrailEntry from_file[kTrailCapacity];
    const size_t file_count = Breadcrumb_TrailParse(page.data(), page.size(), from_file, kTrailCapacity);
    CHECK(file_count == count + 1);
    for (size_t i = 0; i < count; ++i) {  // field by field: the structs have padding
        CHECK(from_file[i].qpc == entries[i].qpc && from_file[i].thread_id == entries[i].thread_id &&
              from_file[i].extra == entries[i].extra && from_file[i].kind == entries[i].kind &&
              from_file[i].site == entries[i].site && from_file[i].aux == entries[i].aux);
    }
    // A page without the header is not a trail; a short one neither.
    std::vector<unsigned char> blank(kBreadcrumbFileSize, 0);
    CHECK(Breadcrumb_TrailParse(blank.data(), blank.size(), from_file, kTrailCapacity) == 0);
    CHECK(Breadcrumb_TrailParse(page.data(), 100, from_file, kTrailCapacity) == 0);
    // The record at offset 0 is a valid record next to the trail.
    BreadcrumbRecord record;
    std::memcpy(&record, page.data(), sizeof(record));
    CHECK(record.magic == kBreadcrumbMagic && record.seq_begin == record.seq_end && (record.seq_begin & 1u) == 0);

    // A reload in the same process resets the trail with the record.
    CHECK(!Breadcrumb_Attach(path.c_str()).report);  // Closed: no report
    CHECK(Breadcrumb_TrailSnapshot(entries, kTrailCapacity) == 0);
    Breadcrumb_Mark(StartupStage::InitBegun);
    CHECK(Breadcrumb_TrailSnapshot(entries, kTrailCapacity) == 1);
    Breadcrumb_Detach();
    CHECK(Breadcrumb_TrailSnapshot(entries, kTrailCapacity) == 0);
    Remove(path);
}

void TestTrailWrapAndThreads() {
    const std::string path = TempPath("wrap");
    Remove(path);
    CHECK(!Breadcrumb_Attach(path.c_str()).report);
    TrailEntry entries[kTrailCapacity];

    // Wrap: the newest 96 of 250 appends, oldest first, consecutive.
    for (uint32_t i = 0; i < 250; ++i) Breadcrumb_Trail(TrailKind::QueueOfferEnter, 0, i);
    size_t count = Breadcrumb_TrailSnapshot(entries, kTrailCapacity);
    CHECK(count == kTrailCapacity);
    for (size_t i = 0; i < count; ++i) CHECK(entries[i].extra == 250 - kTrailCapacity + i);
    // A smaller buffer gets the newest entries.
    TrailEntry few[10];
    CHECK(Breadcrumb_TrailSnapshot(few, 10) == 10);
    for (size_t i = 0; i < 10; ++i) CHECK(few[i].extra == 240 + i);

    // An entry from a second thread carries that thread's id.
    DWORD other_id = 0;
    std::thread other([&] {
        other_id = GetCurrentThreadId();
        Breadcrumb_Trail(TrailKind::FirstReflexSleep, 7, 0xABCD1234u, 9);
    });
    other.join();
    count = Breadcrumb_TrailSnapshot(entries, kTrailCapacity);
    CHECK(entries[count - 1].thread_id == other_id && other_id != GetCurrentThreadId());
    CHECK(entries[count - 1].kind == static_cast<uint8_t>(TrailKind::FirstReflexSleep) && entries[count - 1].site == 7 &&
          entries[count - 1].extra == 0xABCD1234u && entries[count - 1].aux == 9);

    // Writers racing readers: every complete slot a reader takes holds one writer's whole entry (the site is the
    // writer's number, the extra's top byte too), and the record at offset 0 is not disturbed.
    std::atomic<bool> stop{false};
    std::vector<std::thread> writers;
    for (uint32_t t = 1; t <= 4; ++t) {
        writers.emplace_back([t, &stop] {
            uint32_t i = 0;
            while (!stop.load(std::memory_order_relaxed) && i < 200000)
                Breadcrumb_Trail(TrailKind::FreezeLeave, static_cast<uint8_t>(t), (t << 24) | (i++ & 0xFFFFFF), static_cast<uint16_t>(t));
        });
    }
    for (int round = 0; round < 3000; ++round) {
        count = Breadcrumb_TrailSnapshot(entries, kTrailCapacity);
        for (size_t i = 0; i < count; ++i) {
            CHECK(entries[i].kind != 0 && entries[i].kind <= kTrailKindLast);
            if (entries[i].kind == static_cast<uint8_t>(TrailKind::FreezeLeave)) {
                CHECK(entries[i].site == (entries[i].extra >> 24) && entries[i].aux == entries[i].site);
            }
        }
    }
    stop.store(true);
    for (auto& writer : writers) writer.join();
    BreadcrumbRecord record;
    CHECK(ReadRecord(path, &record));
    CHECK(record.stage == static_cast<uint8_t>(StartupStage::None) && record.seq_begin == record.seq_end);
    Breadcrumb_Detach();
    Remove(path);
}

} // namespace

// The report off (hang_report_state.h, the default): the start-up record keeps working, the trail records nothing.
void TestTrailOffIsInert() {
    CHECK(!HangReport_Active());
    const std::string path = TempPath("trailoff");
    Remove(path);
    CHECK(!Breadcrumb_Attach(path.c_str()).report);
    Breadcrumb_Mark(StartupStage::InitBegun);
    Breadcrumb_Mark(StartupStage::BatchApplyEntered, 1);
    Breadcrumb_Trail(TrailKind::FreezeEnter, 4, 0, 1);
    std::atomic<bool> first{false};
    Breadcrumb_TrailFirst(first, TrailKind::FirstReflexSleep);
    CHECK(!first.load());   // the site's own flag is never stored while the report is off
    TrailEntry entries[kTrailCapacity];
    CHECK(Breadcrumb_TrailSnapshot(entries, kTrailCapacity) == 0);
    CHECK(StageIs(path, StartupStage::BatchApplyEntered, 1));   // the record itself is unaffected
    Breadcrumb_Detach();
    Remove(path);
}

int main() {
    TestTrailOffIsInert();
    HangReport_SetActive(true);   // the trail tests below run with the report on
    TestJudge();
    TestDescribe();
    TestWriteKillRead();
    TestFirstWinsAndBatches();
    TestRunningAndClosed();
    TestReloadInProcess();
    TestUnusableRecords();
    TestUnusablePaths();
    TestConcurrentMarks();
    TestTrail();
    TestTrailWrapAndThreads();
    std::puts("test_startup_breadcrumb: all tests passed");
    return 0;
}
