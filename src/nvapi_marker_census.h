#pragma once
#include <array>
#include <atomic>
#include <cstdint>

// Process-lifetime diagnostic counts, never reset across live producers.
// Each sample is coherent; counts across stages are not an atomic snapshot.
enum class MarkerCensusStage : uint32_t { Entry, Admitted, Rejected, Enforced, Count };
struct MarkerCensusSample {
    uint64_t frame=0,device=0,caller=0;
    int64_t qpc=0;
    uint32_t thread=0,producer=0,reason=0,selected=0;
};
class NvApiMarkerCensus {
public:
    static constexpr uint32_t Types=7; // types0..5 plus other
    struct Snapshot { uint64_t count=0,misses=0;MarkerCensusSample first{},last{}; };
    void Record(uint32_t type,MarkerCensusStage stage,const MarkerCensusSample& sample) noexcept {
        auto& cell=cells_[(type<6?type:6)*4+static_cast<uint32_t>(stage)];
        cell.count.fetch_add(1,std::memory_order_relaxed);
        if(cell.busy.test_and_set(std::memory_order_acquire)) {
            cell.misses.fetch_add(1,std::memory_order_relaxed);return;
        }
        if(!cell.first.qpc)cell.first=sample;
        cell.last=sample;
        cell.busy.clear(std::memory_order_release);
    }
    bool Read(uint32_t type,MarkerCensusStage stage,Snapshot* out) noexcept {
        auto& cell=cells_[type*4+static_cast<uint32_t>(stage)];
        if(cell.busy.test_and_set(std::memory_order_acquire))return false;
        *out={cell.count.load(std::memory_order_relaxed),cell.misses.load(std::memory_order_relaxed),cell.first,cell.last};
        cell.busy.clear(std::memory_order_release);return true;
    }
private:
    struct Cell {
        std::atomic<uint64_t> count{0},misses{0};
        std::atomic_flag busy=ATOMIC_FLAG_INIT;
        MarkerCensusSample first{},last{};
    };
    std::array<Cell,Types*4> cells_{};
};
inline NvApiMarkerCensus g_nvapi_marker_census;