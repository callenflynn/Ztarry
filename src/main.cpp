#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <netioapi.h>
#include <iphlpapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <curses.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace {

enum class SortMode {
    Cpu = 0,
    Ram,
    Disk,
    Gpu,
    Network,
};

struct ProcPrev {
    ULONGLONG procCpuTime = 0;
    ULONGLONG readBytes = 0;
    ULONGLONG writeBytes = 0;
    ULONGLONG otherBytes = 0;
    ULONGLONG systemCpuTime = 0;
    std::chrono::steady_clock::time_point ts{};
};

struct ProcRow {
    DWORD pid = 0;
    std::string name;
    double cpuPct = 0.0;
    double ramMb = 0.0;
    double diskReadMBps = 0.0;
    double diskWriteMBps = 0.0;
    double gpuPct = 0.0;
    double netMBps = 0.0;
};

struct SysSample {
    double cpuPct = 0.0;
    double ramPct = 0.0;
    double ramUsedGb = 0.0;
    double ramTotalGb = 0.0;
    double gpuPct = 0.0;
    double netDownMbps = 0.0;
    double netUpMbps = 0.0;
    double diskReadMBps = 0.0;
    double diskWriteMBps = 0.0;
    bool netAvailable = false;
    bool diskAvailable = false;
    bool gpuAvailable = false;
};

ULONGLONG filetime_to_u64(const FILETIME &ft) {
    ULARGE_INTEGER value{};
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

double clamp_0_100(double v) {
    if (v < 0.0) return 0.0;
    if (v > 100.0) return 100.0;
    return v;
}

ULONGLONG safe_delta_u64(ULONGLONG current, ULONGLONG previous) {
    return current >= previous ? (current - previous) : 0;
}

class CpuSampler {
public:
    double sample() {
        FILETIME idle{}, kernel{}, user{};
        if (!GetSystemTimes(&idle, &kernel, &user)) {
            return 0.0;
        }

        const ULONGLONG idleNow = filetime_to_u64(idle);
        const ULONGLONG kernelNow = filetime_to_u64(kernel);
        const ULONGLONG userNow = filetime_to_u64(user);

        if (!initialized_) {
            idlePrev_ = idleNow;
            kernelPrev_ = kernelNow;
            userPrev_ = userNow;
            initialized_ = true;
            return 0.0;
        }

        const ULONGLONG idleDelta = idleNow - idlePrev_;
        const ULONGLONG kernelDelta = kernelNow - kernelPrev_;
        const ULONGLONG userDelta = userNow - userPrev_;
        const ULONGLONG totalDelta = kernelDelta + userDelta;

        idlePrev_ = idleNow;
        kernelPrev_ = kernelNow;
        userPrev_ = userNow;

        if (totalDelta == 0) {
            return 0.0;
        }

        const double busy = static_cast<double>(totalDelta - idleDelta);
        return clamp_0_100((busy * 100.0) / static_cast<double>(totalDelta));
    }

    ULONGLONG raw_system_cpu_time() const {
        FILETIME idle{}, kernel{}, user{};
        if (!GetSystemTimes(&idle, &kernel, &user)) {
            return 0;
        }
        return filetime_to_u64(kernel) + filetime_to_u64(user);
    }

private:
    bool initialized_ = false;
    ULONGLONG idlePrev_ = 0;
    ULONGLONG kernelPrev_ = 0;
    ULONGLONG userPrev_ = 0;
};

class NetSampler {
public:
    bool sample(double &downMbps, double &upMbps) {
        downMbps = 0.0;
        upMbps = 0.0;

        MIB_IF_TABLE2 *table = nullptr;
        if (GetIfTable2(&table) != NO_ERROR || table == nullptr) {
            return false;
        }

        ULONGLONG inTotal = 0;
        ULONGLONG outTotal = 0;

        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const MIB_IF_ROW2 &row = table->Table[i];
            if (row.OperStatus != IfOperStatusUp) {
                continue;
            }
            if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK || row.Type == IF_TYPE_TUNNEL) {
                continue;
            }
            inTotal += row.InOctets;
            outTotal += row.OutOctets;
        }

        FreeMibTable(table);

        const auto now = std::chrono::steady_clock::now();
        if (initialized_) {
            const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - prevTs_).count();
            if (dtMs > 0) {
                const double dt = static_cast<double>(dtMs) / 1000.0;
                const ULONGLONG inDelta = safe_delta_u64(inTotal, prevIn_);
                const ULONGLONG outDelta = safe_delta_u64(outTotal, prevOut_);
                downMbps = (static_cast<double>(inDelta) * 8.0) / (dt * 1000.0 * 1000.0);
                upMbps = (static_cast<double>(outDelta) * 8.0) / (dt * 1000.0 * 1000.0);
            }
        }

        prevIn_ = inTotal;
        prevOut_ = outTotal;
        prevTs_ = now;
        initialized_ = true;
        return true;
    }

private:
    bool initialized_ = false;
    ULONGLONG prevIn_ = 0;
    ULONGLONG prevOut_ = 0;
    std::chrono::steady_clock::time_point prevTs_{};
};

class PdhSampler {
public:
    bool init() {
        if (initialized_) {
            return true;
        }

        if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
            return false;
        }

        add_counter(L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", diskReadCounter_);
        add_counter(L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", diskWriteCounter_);

        // Wildcard GPU engine counter. Values are summed by pid and globally.
        add_counter(L"\\GPU Engine(*)\\Utilization Percentage", gpuEngineCounter_);

        PdhCollectQueryData(query_);
        initialized_ = true;
        return true;
    }

    bool sample_all(
        double &readMBps,
        double &writeMBps,
        double &totalGpuPct,
        std::unordered_map<DWORD, double> &perPidGpu) {

        readMBps = 0.0;
        writeMBps = 0.0;
        totalGpuPct = 0.0;
        perPidGpu.clear();

        if (!initialized_) return false;
        if (PdhCollectQueryData(query_) != ERROR_SUCCESS) {
            return false;
        }

        PDH_FMT_COUNTERVALUE readValue{};
        PDH_FMT_COUNTERVALUE writeValue{};
        bool anyCounterOk = false;

        if (diskReadCounter_ &&
            PdhGetFormattedCounterValue(diskReadCounter_, PDH_FMT_DOUBLE, nullptr, &readValue) == ERROR_SUCCESS) {
            readMBps = readValue.doubleValue / (1024.0 * 1024.0);
            anyCounterOk = true;
        }

        if (diskWriteCounter_ &&
            PdhGetFormattedCounterValue(diskWriteCounter_, PDH_FMT_DOUBLE, nullptr, &writeValue) == ERROR_SUCCESS) {
            writeMBps = writeValue.doubleValue / (1024.0 * 1024.0);
            anyCounterOk = true;
        }

        if (!initialized_ || gpuEngineCounter_ == nullptr) {
            return anyCounterOk;
        }

        DWORD bytesNeeded = 0;
        DWORD itemCount = 0;
        const PDH_STATUS first = PdhGetFormattedCounterArrayW(
            gpuEngineCounter_,
            PDH_FMT_DOUBLE,
            &bytesNeeded,
            &itemCount,
            nullptr);

        if (first != PDH_MORE_DATA || bytesNeeded == 0) {
            return anyCounterOk;
        }

        std::vector<BYTE> buffer(bytesNeeded);
        auto *items = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buffer.data());

        if (PdhGetFormattedCounterArrayW(
                gpuEngineCounter_,
                PDH_FMT_DOUBLE,
                &bytesNeeded,
                &itemCount,
                items) != ERROR_SUCCESS) {
            return anyCounterOk;
        }

        for (DWORD i = 0; i < itemCount; ++i) {
            const std::wstring instance = items[i].szName ? items[i].szName : L"";
            const double value = std::max(0.0, items[i].FmtValue.doubleValue);
            totalGpuPct += value;

            const DWORD pid = parse_pid(instance);
            if (pid != 0) {
                perPidGpu[pid] += value;
            }
        }

        totalGpuPct = clamp_0_100(totalGpuPct);
        for (auto &kv : perPidGpu) {
            kv.second = clamp_0_100(kv.second);
        }

        return true;
    }

    ~PdhSampler() {
        if (query_) {
            PdhCloseQuery(query_);
        }
    }

private:
    static DWORD parse_pid(const std::wstring &instance) {
        const std::wstring key = L"pid_";
        const size_t start = instance.find(key);
        if (start == std::wstring::npos) return 0;

        size_t pos = start + key.size();
        size_t end = pos;
        while (end < instance.size() && instance[end] >= L'0' && instance[end] <= L'9') {
            ++end;
        }

        if (end == pos) return 0;

        try {
            return static_cast<DWORD>(std::stoul(instance.substr(pos, end - pos)));
        } catch (...) {
            return 0;
        }
    }

    void add_counter(const wchar_t *path, PDH_HCOUNTER &counter) {
        counter = nullptr;
        if (!query_) return;

        PDH_STATUS st = PdhAddEnglishCounterW(query_, path, 0, &counter);
        if (st != ERROR_SUCCESS) {
            st = PdhAddCounterW(query_, path, 0, &counter);
            if (st != ERROR_SUCCESS) {
                counter = nullptr;
            }
        }
    }

    bool initialized_ = false;
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER diskReadCounter_ = nullptr;
    PDH_HCOUNTER diskWriteCounter_ = nullptr;
    PDH_HCOUNTER gpuEngineCounter_ = nullptr;
};

std::string truncate_text(const std::string &s, size_t maxLen) {
    if (s.size() <= maxLen) return s;
    if (maxLen <= 3) return s.substr(0, maxLen);
    return s.substr(0, maxLen - 3) + "...";
}

std::string utf8_from_wide(const wchar_t *wstr) {
    if (wstr == nullptr) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

std::string make_bar(double pct, int width) {
    const int fill = static_cast<int>((clamp_0_100(pct) / 100.0) * width);
    std::string out;
    out.reserve(static_cast<size_t>(width));
    for (int i = 0; i < width; ++i) {
        out.push_back(i < fill ? '#' : '.');
    }
    return out;
}

void draw_box(int y, int x, int h, int w, const std::string &title, int colorPair) {
    if (h < 2 || w < 2) return;

    attron(COLOR_PAIR(colorPair));

    mvaddch(y, x, '+');
    mvhline(y, x + 1, '-', w - 2);
    mvaddch(y, x + w - 1, '+');

    for (int row = y + 1; row < y + h - 1; ++row) {
        mvaddch(row, x, '|');
        mvaddch(row, x + w - 1, '|');
    }

    mvaddch(y + h - 1, x, '+');
    mvhline(y + h - 1, x + 1, '-', w - 2);
    mvaddch(y + h - 1, x + w - 1, '+');

    if (!title.empty() && w > 4) {
        const std::string t = " " + title + " ";
        mvprintw(y, x + 2, "%s", t.c_str());
    }

    attroff(COLOR_PAIR(colorPair));
}

std::vector<ProcRow> collect_processes(
    std::unordered_map<DWORD, ProcPrev> &prevMap,
    const std::unordered_map<DWORD, double> &gpuByPid,
    ULONGLONG nowSystemCpuTime) {

    std::vector<ProcRow> rows;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return rows;
    }

    std::unordered_map<DWORD, bool> seen;
    seen.reserve(prevMap.size() + 512);

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap, &pe)) {
        CloseHandle(snap);
        return rows;
    }

    const auto now = std::chrono::steady_clock::now();

    do {
        const DWORD pid = pe.th32ProcessID;
        seen[pid] = true;

        HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!ph) {
            continue;
        }

        PROCESS_MEMORY_COUNTERS pmc{};
        double ramMb = 0.0;
        if (GetProcessMemoryInfo(ph, &pmc, sizeof(pmc))) {
            ramMb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        }

        FILETIME create{}, exitTime{}, kernel{}, user{};
        ULONGLONG procCpu = 0;
        if (GetProcessTimes(ph, &create, &exitTime, &kernel, &user)) {
            procCpu = filetime_to_u64(kernel) + filetime_to_u64(user);
        }

        IO_COUNTERS io{};
        ULONGLONG readBytes = 0;
        ULONGLONG writeBytes = 0;
        ULONGLONG otherBytes = 0;
        if (GetProcessIoCounters(ph, &io)) {
            readBytes = io.ReadTransferCount;
            writeBytes = io.WriteTransferCount;
            otherBytes = io.OtherTransferCount;
        }

        auto &prev = prevMap[pid];

        double cpuPct = 0.0;
        double diskReadMBps = 0.0;
        double diskWriteMBps = 0.0;
        double netMBps = 0.0;

        if (prev.ts.time_since_epoch().count() != 0 && nowSystemCpuTime > prev.systemCpuTime) {
            const ULONGLONG procDelta = procCpu - prev.procCpuTime;
            const ULONGLONG sysDelta = nowSystemCpuTime - prev.systemCpuTime;
            if (sysDelta > 0) {
                cpuPct = (static_cast<double>(procDelta) * 100.0) / static_cast<double>(sysDelta);
                cpuPct = clamp_0_100(cpuPct);
            }

            const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - prev.ts).count();
            if (dtMs > 0) {
                const double dt = static_cast<double>(dtMs) / 1000.0;
                diskReadMBps = (static_cast<double>(safe_delta_u64(readBytes, prev.readBytes)) / (1024.0 * 1024.0)) / dt;
                diskWriteMBps = (static_cast<double>(safe_delta_u64(writeBytes, prev.writeBytes)) / (1024.0 * 1024.0)) / dt;
                netMBps = (static_cast<double>(safe_delta_u64(otherBytes, prev.otherBytes)) / (1024.0 * 1024.0)) / dt;
            }
        }

        prev.procCpuTime = procCpu;
        prev.readBytes = readBytes;
        prev.writeBytes = writeBytes;
        prev.otherBytes = otherBytes;
        prev.systemCpuTime = nowSystemCpuTime;
        prev.ts = now;

        ProcRow row;
        row.pid = pid;
        row.name = utf8_from_wide(pe.szExeFile);
        row.cpuPct = std::max(0.0, cpuPct);
        row.ramMb = std::max(0.0, ramMb);
        row.diskReadMBps = std::max(0.0, diskReadMBps);
        row.diskWriteMBps = std::max(0.0, diskWriteMBps);
        row.netMBps = std::max(0.0, netMBps);

        auto gpuIt = gpuByPid.find(pid);
        row.gpuPct = gpuIt != gpuByPid.end() ? clamp_0_100(gpuIt->second) : 0.0;

        rows.push_back(row);

        CloseHandle(ph);

    } while (Process32NextW(snap, &pe));

    CloseHandle(snap);

    std::vector<DWORD> stale;
    stale.reserve(prevMap.size());
    for (const auto &kv : prevMap) {
        if (seen.find(kv.first) == seen.end()) {
            stale.push_back(kv.first);
        }
    }
    for (DWORD pid : stale) {
        prevMap.erase(pid);
    }

    return rows;
}

void sort_rows(std::vector<ProcRow> &rows, SortMode mode) {
    std::sort(rows.begin(), rows.end(), [mode](const ProcRow &a, const ProcRow &b) {
        switch (mode) {
            case SortMode::Cpu:
                return a.cpuPct > b.cpuPct;
            case SortMode::Ram:
                return a.ramMb > b.ramMb;
            case SortMode::Disk:
                return (a.diskReadMBps + a.diskWriteMBps) > (b.diskReadMBps + b.diskWriteMBps);
            case SortMode::Gpu:
                return a.gpuPct > b.gpuPct;
            case SortMode::Network:
                return a.netMBps > b.netMBps;
        }
        return false;
    });
}

SysSample sample_system(CpuSampler &cpuSampler, NetSampler &netSampler) {
    SysSample sample;
    sample.cpuPct = cpuSampler.sample();

    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        sample.ramPct = static_cast<double>(mem.dwMemoryLoad);
        sample.ramTotalGb = static_cast<double>(mem.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
        sample.ramUsedGb = static_cast<double>(mem.ullTotalPhys - mem.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
    }

    sample.netAvailable = netSampler.sample(sample.netDownMbps, sample.netUpMbps);

    return sample;
}

}  // namespace

int main() {
    CpuSampler cpuSampler;
    NetSampler netSampler;
    PdhSampler pdhSampler;
    pdhSampler.init();

    std::unordered_map<DWORD, ProcPrev> prevProc;
    prevProc.reserve(2048);

    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_CYAN, -1);
        init_pair(2, COLOR_YELLOW, -1);
        init_pair(3, COLOR_GREEN, -1);
        init_pair(4, COLOR_WHITE, COLOR_BLUE);
        init_pair(5, COLOR_WHITE, -1);
        init_pair(6, COLOR_BLACK, COLOR_YELLOW);
    }

    const std::array<std::string, 5> sortNames = {"CPU", "RAM", "DISK", "GPU", "NETWORK"};
    SortMode activeSort = SortMode::Cpu;
    int sortCursor = 0;
    int selectedRow = 0;
    int listScroll = 0;

    std::deque<double> ramHistory;
    constexpr size_t kHistoryMax = 72;

    bool running = true;
    while (running) {
        int ch = curses_getch();
        switch (ch) {
            case KEY_LEFT:
                sortCursor = (sortCursor - 1 + static_cast<int>(sortNames.size())) % static_cast<int>(sortNames.size());
                break;
            case KEY_RIGHT:
                sortCursor = (sortCursor + 1) % static_cast<int>(sortNames.size());
                break;
            case KEY_UP:
                selectedRow = std::max(0, selectedRow - 1);
                break;
            case KEY_DOWN:
                selectedRow = selectedRow + 1;
                break;
            case 10:
            case KEY_ENTER:
                activeSort = static_cast<SortMode>(sortCursor);
                break;
            case 'q':
            case 'Q':
                running = false;
                break;
            default:
                break;
        }

        double diskReadMBps = 0.0;
        double diskWriteMBps = 0.0;
        double sysGpu = 0.0;
        std::unordered_map<DWORD, double> gpuByPid;
        const bool pdhOk = pdhSampler.sample_all(
            diskReadMBps,
            diskWriteMBps,
            sysGpu,
            gpuByPid);

        SysSample sys = sample_system(cpuSampler, netSampler);
        sys.diskReadMBps = diskReadMBps;
        sys.diskWriteMBps = diskWriteMBps;
        sys.gpuPct = sysGpu;
        sys.diskAvailable = pdhOk;
        sys.gpuAvailable = pdhOk;

        const ULONGLONG rawSystemCpu = cpuSampler.raw_system_cpu_time();
        std::vector<ProcRow> rows = collect_processes(prevProc, gpuByPid, rawSystemCpu);
        sort_rows(rows, activeSort);

        if (rows.empty()) {
            selectedRow = 0;
        } else {
            selectedRow = std::min(selectedRow, static_cast<int>(rows.size()) - 1);
        }

        ramHistory.push_back(sys.ramPct);
        if (ramHistory.size() > kHistoryMax) {
            ramHistory.pop_front();
        }

        erase();

        const int h = LINES;
        const int w = COLS;

        const int topH = std::min(11, h - 6);
        const int listY = topH;
        const int listH = h - listY;

        draw_box(0, 0, topH, w, " Ztarry ", 1);
        draw_box(listY, 0, listH, w, " Programs ", 1);

        attron(COLOR_PAIR(2));
        mvprintw(1, 2, "Sort:");
        int sx = 8;
        for (int i = 0; i < static_cast<int>(sortNames.size()); ++i) {
            const bool isCursor = i == sortCursor;
            const bool isActive = static_cast<int>(activeSort) == i;
            if (isCursor) attron(COLOR_PAIR(6));
            if (isActive) attron(A_BOLD);
            mvprintw(1, sx, "[%s]", sortNames[i].c_str());
            if (isCursor) attroff(COLOR_PAIR(6));
            if (isActive) attroff(A_BOLD);
            sx += static_cast<int>(sortNames[i].size()) + 3;
        }
        mvprintw(1, std::max(2, w - 28), "Arrows: Navigate  Enter: Apply  Q: Quit");
        attroff(COLOR_PAIR(2));

        attron(COLOR_PAIR(3));
        mvprintw(3, 2, "CPU: %5.1f%%  [%s]", sys.cpuPct, make_bar(sys.cpuPct, 22).c_str());
        if (sys.gpuAvailable) {
            mvprintw(4, 2, "GPU: %5.1f%%  [%s]", sys.gpuPct, make_bar(sys.gpuPct, 22).c_str());
        } else {
            mvprintw(4, 2, "GPU:    N/A   [......................]");
        }
        mvprintw(5, 2, "RAM: %5.1f%%  (%4.1f / %4.1f GB)", sys.ramPct, sys.ramUsedGb, sys.ramTotalGb);
        if (sys.netAvailable) {
            mvprintw(6, 2, "NET: D %8.2f Mbps   U %8.2f Mbps", sys.netDownMbps, sys.netUpMbps);
        } else {
            mvprintw(6, 2, "NET: D      N/A      U      N/A   ");
        }
        if (sys.diskAvailable) {
            mvprintw(7, 2, "DSK: R %8.2f MB/s   W %8.2f MB/s", sys.diskReadMBps, sys.diskWriteMBps);
        } else {
            mvprintw(7, 2, "DSK: R      N/A      W      N/A   ");
        }
        attroff(COLOR_PAIR(3));

        static const char *kLevels = " .:-=+*#%@";
        mvprintw(8, 2, "RAM Graph:");
        int gx = 13;
        for (double v : ramHistory) {
            const int level = std::min(9, std::max(0, static_cast<int>(v / 10.0)));
            if (gx < w - 2) {
                mvaddch(8, gx++, kLevels[level]);
            }
        }

        mvprintw(listY + 1, 2, "%1s %-22s %6s %8s %9s %6s %8s", " ", "Name", "CPU%", "RAM MB", "DISK", "GPU%", "NET");
        mvprintw(listY + 2, 2, "%1s %-22s %6s %8s %9s %6s %8s", " ", "----------------------", "------", "--------", "---------", "------", "--------");

        const int rowsStart = listY + 3;
        const int rowsVisible = std::max(0, h - rowsStart - 1);

        if (selectedRow < listScroll) {
            listScroll = selectedRow;
        }
        if (selectedRow >= listScroll + rowsVisible) {
            listScroll = selectedRow - rowsVisible + 1;
        }
        listScroll = std::max(0, listScroll);

        for (int i = 0; i < rowsVisible; ++i) {
            const int idx = listScroll + i;
            if (idx >= static_cast<int>(rows.size())) break;

            const ProcRow &r = rows[idx];
            const bool selected = idx == selectedRow;
            const int rowY = rowsStart + i;

            if (selected) {
                attron(COLOR_PAIR(4) | A_BOLD);
                if (w > 2) {
                    mvhline(rowY, 1, ' ', w - 2);
                }
            } else {
                attron(COLOR_PAIR(5));
            }

            const std::string name = truncate_text(r.name, 22);
            const double diskTotal = r.diskReadMBps + r.diskWriteMBps;
            mvprintw(rowY, 2, "%c", selected ? '>' : ' ');
            mvprintw(rowY, 4,
                     "%-22s %6.1f %8.0f %9.2f %6.1f %8.2f",
                     name.c_str(),
                     r.cpuPct,
                     r.ramMb,
                     diskTotal,
                     r.gpuPct,
                     r.netMBps);

            if (selected) {
                attroff(COLOR_PAIR(4) | A_BOLD);
            } else {
                attroff(COLOR_PAIR(5));
            }
        }

        wnoutrefresh(stdscr);
        doupdate();

        std::this_thread::sleep_for(std::chrono::milliseconds(900));
    }

    endwin();
    return 0;
}
