#include "game/warden_emulator.hpp"
#include "core/logger.hpp"
#include <cstring>
#include <chrono>
#include <iterator>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>

#ifdef HAVE_UNICORN
// Unicorn Engine headers
#include <unicorn/unicorn.h>
#endif

namespace wowee {
namespace game {

#ifdef HAVE_UNICORN

namespace {

std::string lowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

// Memory layout for emulated environment
// Note: heap must not overlap the module region (typically loaded at 0x400000)
// or the stack. Keep heap above 0x02000000 (32MB) to leave space for module + padding.
constexpr uint32_t STACK_BASE = 0x00100000;  // 1MB
constexpr uint32_t STACK_SIZE = 0x00100000;  // 1MB stack
constexpr uint32_t HEAP_BASE  = 0x02000000;  // 32MB — well above typical module base (0x400000)
constexpr uint32_t HEAP_SIZE  = 0x01000000;  // 16MB heap
constexpr uint32_t API_STUB_BASE = 0x70000000; // API stub area (high memory)

WardenEmulator::WardenEmulator()
    : uc_(nullptr)
    , moduleBase_(0)
    , moduleSize_(0)
    , stackBase_(STACK_BASE)
    , stackSize_(STACK_SIZE)
    , heapBase_(HEAP_BASE)
    , heapSize_(HEAP_SIZE)
    , apiStubBase_(API_STUB_BASE)
    , nextApiStubAddr_(API_STUB_BASE)
    , apiCodeHookRegistered_(false)
    , nextHeapAddr_(HEAP_BASE)
    , lastCodeAddress_(0)
    , lastCodeSize_(0)
    , nextTlsIndex_(1)
{
}

WardenEmulator::~WardenEmulator() {
    if (uc_) {
        uc_close(uc_);
    }
}

bool WardenEmulator::initialize(const void* moduleCode, size_t moduleSize, uint32_t baseAddress) {
    if (uc_) {
        LOG_ERROR("WardenEmulator: Already initialized");
        return false;
    }
    // Reset allocator state so re-initialization starts with a clean heap.
    allocations_.clear();
    freeBlocks_.clear();
    apiAddresses_.clear();
    apiHandlers_.clear();
    hooks_.clear();
    nextHeapAddr_ = heapBase_;
    nextApiStubAddr_ = apiStubBase_;
    apiCodeHookRegistered_ = false;
    lastCodeAddress_ = 0;
    lastCodeSize_ = 0;
    nextTlsIndex_ = 1;
    tlsValues_.clear();

    {
        char addrBuf[32];
        std::snprintf(addrBuf, sizeof(addrBuf), "0x%X", baseAddress);
        LOG_INFO("WardenEmulator: Initializing x86 emulator (Unicorn Engine)");
        LOG_INFO("WardenEmulator:   Module: ", moduleSize, " bytes at ", addrBuf);
    }

    // Create x86 32-bit emulator
    uc_err err = uc_open(UC_ARCH_X86, UC_MODE_32, &uc_);
    if (err != UC_ERR_OK) {
        LOG_ERROR("WardenEmulator: uc_open failed: ", uc_strerror(err));
        return false;
    }

    moduleBase_ = baseAddress;
    moduleSize_ = (moduleSize + 0xFFF) & ~0xFFF; // Align to 4KB

    // Detect overlap between module and heap/stack regions early.
    uint32_t modEnd = moduleBase_ + moduleSize_;
    if (modEnd > heapBase_ && moduleBase_ < heapBase_ + heapSize_) {
        {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "WardenEmulator: Module [0x%X, 0x%X) overlaps heap [0x%X, 0x%X) - adjust HEAP_BASE",
                          moduleBase_, modEnd, heapBase_, heapBase_ + heapSize_);
            LOG_ERROR(buf);
        }
        uc_close(uc_);
        uc_ = nullptr;
        return false;
    }

    // Map module memory (code + data)
    err = uc_mem_map(uc_, moduleBase_, moduleSize_, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        LOG_ERROR("WardenEmulator: Failed to map module memory: ", uc_strerror(err));
        uc_close(uc_);
        uc_ = nullptr;
        return false;
    }

    // Write module code to emulated memory
    err = uc_mem_write(uc_, moduleBase_, moduleCode, moduleSize);
    if (err != UC_ERR_OK) {
        LOG_ERROR("WardenEmulator: Failed to write module code: ", uc_strerror(err));
        uc_close(uc_);
        uc_ = nullptr;
        return false;
    }

    // Map stack
    err = uc_mem_map(uc_, stackBase_, stackSize_, UC_PROT_READ | UC_PROT_WRITE);
    if (err != UC_ERR_OK) {
        LOG_ERROR("WardenEmulator: Failed to map stack: ", uc_strerror(err));
        uc_close(uc_);
        uc_ = nullptr;
        return false;
    }

    // Initialize stack pointer (grows downward)
    uint32_t esp = stackBase_ + stackSize_ - 0x1000; // Leave some space at top
    uc_reg_write(uc_, UC_X86_REG_ESP, &esp);
    uc_reg_write(uc_, UC_X86_REG_EBP, &esp);

    // Map heap
    err = uc_mem_map(uc_, heapBase_, heapSize_, UC_PROT_READ | UC_PROT_WRITE);
    if (err != UC_ERR_OK) {
        LOG_ERROR("WardenEmulator: Failed to map heap: ", uc_strerror(err));
        uc_close(uc_);
        uc_ = nullptr;
        return false;
    }

    // Map API stub area
    err = uc_mem_map(uc_, apiStubBase_, 0x10000, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        LOG_ERROR("WardenEmulator: Failed to map API stub area: ", uc_strerror(err));
        uc_close(uc_);
        uc_ = nullptr;
        return false;
    }

    // Map a null guard page at address 0 (read-only, zeroed) so that NULL-pointer
    // dereferences in the module don't crash the emulator with UC_ERR_MAP.
    // This allows execution to continue past NULL reads, making diagnostics easier.
    err = uc_mem_map(uc_, 0x0, 0x1000, UC_PROT_READ);
    if (err != UC_ERR_OK) {
        // Non-fatal — just log it; the emulator will still function
        LOG_WARNING("WardenEmulator: could not map null guard page: ", uc_strerror(err));
    }

    // Add hooks for debugging and invalid memory access
    uc_hook hh;
    uc_hook_add(uc_, &hh, UC_HOOK_MEM_INVALID, (void*)hookMemInvalid, this, 1, 0);
    hooks_.push_back(hh);

    // Track module execution for diagnostics. The hook only records the last
    // address unless it lands on an API stub, so normal execution stays quiet.
    uc_hook moduleHook;
    uc_hook_add(uc_, &moduleHook, UC_HOOK_CODE, (void*)hookCode, this,
                moduleBase_, moduleBase_ + moduleSize_ - 1);
    hooks_.push_back(moduleHook);

    // Add code hook over the API stub area so Windows API calls are intercepted.
    uc_hook apiHook;
    uc_hook_add(uc_, &apiHook, UC_HOOK_CODE, (void*)hookCode, this,
                API_STUB_BASE, API_STUB_BASE + 0x10000 - 1);
    hooks_.push_back(apiHook);
    apiCodeHookRegistered_ = true;

    {
        char sBuf[128];
        std::snprintf(sBuf, sizeof(sBuf), "WardenEmulator: Emulator initialized  Stack: 0x%X-0x%X  Heap: 0x%X-0x%X",
                      stackBase_, stackBase_ + stackSize_, heapBase_, heapBase_ + heapSize_);
        LOG_INFO(sBuf);
    }

    return true;
}

bool WardenEmulator::syncModuleMemory(const void* moduleCode, size_t moduleSize) {
    if (!uc_ || !moduleCode) return false;
    if (moduleSize > moduleSize_) {
        LOG_ERROR("WardenEmulator: syncModuleMemory size exceeds mapping (",
                  moduleSize, " > ", moduleSize_, ")");
        return false;
    }
    uc_err err = uc_mem_write(uc_, moduleBase_, moduleCode, moduleSize);
    if (err != UC_ERR_OK) {
        LOG_ERROR("WardenEmulator: syncModuleMemory failed: ", uc_strerror(err));
        return false;
    }
    LOG_INFO("WardenEmulator: Synced patched module image into emulator (",
             moduleSize, " bytes)");
    return true;
}

uint32_t WardenEmulator::hookAPI(const std::string& dllName,
                                 const std::string& functionName,
                                 std::function<uint32_t(WardenEmulator&, const std::vector<uint32_t>&)> handler) {
    // Determine stdcall arg count from known Windows APIs so the hook can
    // clean up the stack correctly (RETN N convention).
    static const std::pair<const char*, int> knownArgCounts[] = {
        {"VirtualAlloc",           4},
        {"VirtualFree",            3},
        {"GetTickCount",           0},
        {"GetSystemTimeAsFileTime", 1},
        {"QueryPerformanceCounter", 1},
        {"GetModuleHandleA",       1},
        {"LoadLibraryA",           1},
        {"FreeLibrary",            1},
        {"GetProcAddress",         2},
        {"GetSystemInfo",          1},
        {"GetVersionExA",          1},
        {"CreateToolhelp32Snapshot", 2},
        {"Module32First",          2},
        {"Module32Next",           2},
        {"TlsAlloc",               0},
        {"TlsFree",                1},
        {"TlsSetValue",            2},
        {"TlsGetValue",            1},
        {"AddVectoredExceptionHandler",    2},
        {"RemoveVectoredExceptionHandler", 1},
        {"Sleep",                  1},
        {"GetCurrentThreadId",     0},
        {"GetCurrentProcessId",    0},
        {"ReadProcessMemory",      5},
    };
    int argCount = 0;
    for (const auto& [name, cnt] : knownArgCounts) {
        if (functionName == name) { argCount = cnt; break; }
    }

    std::string dllKey = lowerAscii(dllName);
    uint32_t stubAddr = hookFunction(dllName + "!" + functionName, argCount, std::move(handler));

    // Store address mapping for IAT patching
    apiAddresses_[dllKey][functionName] = stubAddr;

    return stubAddr;
}

uint32_t WardenEmulator::hookFunction(
    const std::string& name,
    int argCount,
    std::function<uint32_t(WardenEmulator&, const std::vector<uint32_t>&)> handler) {
    // Allocate address for this API/callback stub (16 bytes each)
    uint32_t stubAddr = nextApiStubAddr_;
    nextApiStubAddr_ += 16;

    // Store the handler so hookCode() can dispatch to it.
    apiHandlers_[stubAddr] = { name, argCount, std::move(handler) };

    // Write a RET (0xC3) at the stub address as a safe fallback in case
    // the code hook fires after EIP has already advanced past our intercept.
    if (uc_) {
        static constexpr uint8_t retInstr = 0xC3;
        uc_mem_write(uc_, stubAddr, &retInstr, 1);
    }

    {
        char hBuf[64];
        std::snprintf(hBuf, sizeof(hBuf), "0x%X (argCount=%d)", stubAddr, argCount);
        LOG_DEBUG("WardenEmulator: Hooked ", name, " at ", hBuf);
    }

    return stubAddr;
}

uint32_t WardenEmulator::getAPIAddress(const std::string& dllName, const std::string& funcName) const {
    auto libIt = apiAddresses_.find(lowerAscii(dllName));
    if (libIt == apiAddresses_.end()) return 0;
    auto funcIt = libIt->second.find(funcName);
    return (funcIt != libIt->second.end()) ? funcIt->second : 0;
}

void WardenEmulator::setupCommonAPIHooks() {
    LOG_INFO("WardenEmulator: Setting up common Windows API hooks...");

    // kernel32.dll
    hookAPI("kernel32.dll", "VirtualAlloc", apiVirtualAlloc);
    hookAPI("kernel32.dll", "VirtualFree", apiVirtualFree);
    hookAPI("kernel32.dll", "GetTickCount", apiGetTickCount);
    hookAPI("kernel32.dll", "GetSystemTimeAsFileTime", apiGetSystemTimeAsFileTime);
    hookAPI("kernel32.dll", "QueryPerformanceCounter", apiQueryPerformanceCounter);
    hookAPI("kernel32.dll", "GetModuleHandleA", apiGetModuleHandleA);
    hookAPI("kernel32.dll", "LoadLibraryA", apiLoadLibraryA);
    hookAPI("kernel32.dll", "FreeLibrary", apiFreeLibrary);
    hookAPI("kernel32.dll", "GetProcAddress", apiGetProcAddress);
    hookAPI("kernel32.dll", "GetSystemInfo", apiGetSystemInfo);
    hookAPI("kernel32.dll", "GetVersionExA", apiGetVersionExA);
    hookAPI("kernel32.dll", "CreateToolhelp32Snapshot", apiCreateToolhelp32Snapshot);
    hookAPI("kernel32.dll", "Module32First", apiModule32First);
    hookAPI("kernel32.dll", "Module32Next", apiModule32Next);
    hookAPI("kernel32.dll", "TlsAlloc", apiTlsAlloc);
    hookAPI("kernel32.dll", "TlsFree", apiTlsFree);
    hookAPI("kernel32.dll", "TlsSetValue", apiTlsSetValue);
    hookAPI("kernel32.dll", "TlsGetValue", apiTlsGetValue);
    hookAPI("kernel32.dll", "AddVectoredExceptionHandler", apiAddVectoredExceptionHandler);
    hookAPI("kernel32.dll", "RemoveVectoredExceptionHandler", apiRemoveVectoredExceptionHandler);
    hookAPI("kernel32.dll", "Sleep", apiSleep);
    hookAPI("kernel32.dll", "GetCurrentThreadId", apiGetCurrentThreadId);
    hookAPI("kernel32.dll", "GetCurrentProcessId", apiGetCurrentProcessId);
    hookAPI("kernel32.dll", "ReadProcessMemory", apiReadProcessMemory);

    LOG_INFO("WardenEmulator: Common API hooks registered");
}

uint32_t WardenEmulator::writeData(const void* data, size_t size) {
    uint32_t addr = allocateMemory(size, 0x04);
    if (addr != 0) {
        if (!writeMemory(addr, data, size)) {
            freeMemory(addr);
            return 0;
        }
    }
    return addr;
}

std::vector<uint8_t> WardenEmulator::readData(uint32_t address, size_t size) {
    std::vector<uint8_t> result(size);
    if (!readMemory(address, result.data(), size)) {
        return {};
    }
    return result;
}

bool WardenEmulator::isRangeMapped(uint32_t address, size_t size) const {
    if (size == 0) return true;
    uint64_t start = address;
    uint64_t end = start + size;
    auto contains = [&](uint32_t base, uint32_t len) {
        uint64_t b = base;
        uint64_t e = b + len;
        return start >= b && end <= e;
    };
    if (contains(moduleBase_, moduleSize_)) return true;
    if (contains(stackBase_, stackSize_)) return true;
    if (contains(heapBase_, heapSize_)) return true;
    if (contains(apiStubBase_, 0x10000)) return true;
    return address == 0 && size <= 0x1000;
}

uint32_t WardenEmulator::callFunction(uint32_t address, const std::vector<uint32_t>& args) {
    if (!uc_) {
        LOG_ERROR("WardenEmulator: Not initialized");
        return 0;
    }

    {
        std::ostringstream oss;
        oss << "WardenEmulator: callFunction entry=0x" << std::hex << address
            << " args=" << std::dec << args.size();
        for (size_t i = 0; i < args.size(); ++i) {
            oss << " arg" << i << "=0x" << std::hex << args[i];
        }
        LOG_WARNING(oss.str());
    }

    if (!isRangeMapped(address, 1)) {
        LOG_ERROR("WardenEmulator: callFunction entry is outside mapped ranges: 0x",
                  std::hex, address, std::dec);
        return 0;
    }

    // Get current ESP. Restore it after the call so both stdcall and cdecl
    // module exports can be invoked without stack drift.
    uint32_t originalEsp;
    uc_reg_read(uc_, UC_X86_REG_ESP, &originalEsp);
    uint32_t esp = originalEsp;

    // Push arguments (stdcall: right-to-left)
    for (auto it = args.rbegin(); it != args.rend(); ++it) {
        esp -= 4;
        uint32_t arg = *it;
        uc_mem_write(uc_, esp, &arg, 4);
    }

    // Push return address (0xFFFFFFFF = terminator)
    uint32_t retAddr = 0xFFFFFFFF;
    esp -= 4;
    uc_mem_write(uc_, esp, &retAddr, 4);

    // Update ESP
    uc_reg_write(uc_, UC_X86_REG_ESP, &esp);
    LOG_WARNING("WardenEmulator: callFunction stack originalESP=0x", std::hex, originalEsp,
                " callESP=0x", esp, " retSentinel=0x", retAddr, std::dec);

    // Execute until return address
    uc_err err = uc_emu_start(uc_, address, retAddr, 0, 0);
    if (err != UC_ERR_OK) {
        LOG_ERROR("WardenEmulator: Execution failed: ", uc_strerror(err));
        uint32_t eip = 0, espNow = 0, ecx = 0, eax = 0;
        uc_reg_read(uc_, UC_X86_REG_EIP, &eip);
        uc_reg_read(uc_, UC_X86_REG_ESP, &espNow);
        uc_reg_read(uc_, UC_X86_REG_ECX, &ecx);
        uc_reg_read(uc_, UC_X86_REG_EAX, &eax);
        LOG_ERROR("WardenEmulator: failure regs EIP=0x", std::hex, eip,
                  " ESP=0x", espNow, " ECX=0x", ecx, " EAX=0x", eax,
                  " last=0x", lastCodeAddress_, "+", lastCodeSize_, std::dec);
        return 0;
    }

    // Get return value (EAX)
    uint32_t eax;
    uint32_t finalEsp = 0;
    uc_reg_read(uc_, UC_X86_REG_EAX, &eax);
    uc_reg_read(uc_, UC_X86_REG_ESP, &finalEsp);
    uc_reg_write(uc_, UC_X86_REG_ESP, &originalEsp);

    LOG_WARNING("WardenEmulator: callFunction returned EAX=0x", std::hex, eax,
                " finalESP=0x", finalEsp, " restoredESP=0x", originalEsp, std::dec);

    return eax;
}

uint32_t WardenEmulator::callThiscall(uint32_t address, uint32_t thisPtr, const std::vector<uint32_t>& args) {
    if (!uc_) {
        LOG_ERROR("WardenEmulator: Not initialized");
        return 0;
    }

    uint32_t originalEcx = 0;
    uc_reg_read(uc_, UC_X86_REG_ECX, &originalEcx);
    uc_reg_write(uc_, UC_X86_REG_ECX, &thisPtr);

    uint32_t result = callFunction(address, args);
    uc_reg_write(uc_, UC_X86_REG_ECX, &originalEcx);
    return result;
}

bool WardenEmulator::readMemory(uint32_t address, void* buffer, size_t size) {
    if (!uc_) return false;
    uc_err err = uc_mem_read(uc_, address, buffer, size);
    return (err == UC_ERR_OK);
}

bool WardenEmulator::writeMemory(uint32_t address, const void* buffer, size_t size) {
    if (!uc_) return false;
    uc_err err = uc_mem_write(uc_, address, buffer, size);
    return (err == UC_ERR_OK);
}

std::string WardenEmulator::readString(uint32_t address, size_t maxLen) {
    std::vector<char> buffer(maxLen + 1, 0);
    if (!readMemory(address, buffer.data(), maxLen)) {
        return "";
    }
    buffer[maxLen] = '\0'; // Ensure null termination
    return std::string(buffer.data());
}

uint32_t WardenEmulator::allocateMemory(size_t size, [[maybe_unused]] uint32_t protection) {
    if (size == 0) return 0;

    // Align to 4KB
    size = (size + 0xFFF) & ~0xFFF;
    const uint32_t allocSize = static_cast<uint32_t>(size);

    // First-fit from free list so released blocks can be reused.
    for (auto it = freeBlocks_.begin(); it != freeBlocks_.end(); ++it) {
        if (it->second < size) continue;
        const uint32_t addr     = it->first;
        const size_t   blockSz  = it->second;
        freeBlocks_.erase(it);
        if (blockSz > size)
            freeBlocks_[addr + allocSize] = blockSz - size;
        allocations_[addr] = size;
        {
            char mBuf[32];
            std::snprintf(mBuf, sizeof(mBuf), "0x%X", addr);
            LOG_DEBUG("WardenEmulator: Reused ", size, " bytes at ", mBuf);
        }
        return addr;
    }

    const uint64_t heapEnd = static_cast<uint64_t>(heapBase_) + heapSize_;
    if (static_cast<uint64_t>(nextHeapAddr_) + size > heapEnd) {
        LOG_ERROR("WardenEmulator: Heap exhausted");
        return 0;
    }

    uint32_t addr = nextHeapAddr_;
    nextHeapAddr_ += allocSize;
    allocations_[addr] = size;

    {
        char mBuf[32];
        std::snprintf(mBuf, sizeof(mBuf), "0x%X", addr);
        LOG_DEBUG("WardenEmulator: Allocated ", size, " bytes at ", mBuf);
    }

    return addr;
}

bool WardenEmulator::freeMemory(uint32_t address) {
    auto it = allocations_.find(address);
    if (it == allocations_.end()) {
        {
            char fBuf[32];
            std::snprintf(fBuf, sizeof(fBuf), "0x%X", address);
            LOG_ERROR("WardenEmulator: Invalid free at ", fBuf);
        }
        return false;
    }

    {
        char fBuf[32];
        std::snprintf(fBuf, sizeof(fBuf), "0x%X", address);
        LOG_DEBUG("WardenEmulator: Freed ", it->second, " bytes at ", fBuf);
    }

    const size_t freedSize = it->second;
    allocations_.erase(it);

    // Insert in free list and coalesce adjacent blocks to limit fragmentation.
    auto [curr, inserted] = freeBlocks_.emplace(address, freedSize);
    if (!inserted) curr->second += freedSize;

    if (curr != freeBlocks_.begin()) {
        auto prev = std::prev(curr);
        if (static_cast<uint64_t>(prev->first) + prev->second == curr->first) {
            prev->second += curr->second;
            freeBlocks_.erase(curr);
            curr = prev;
        }
    }

    auto next = std::next(curr);
    if (next != freeBlocks_.end() &&
        static_cast<uint64_t>(curr->first) + curr->second == next->first) {
        curr->second += next->second;
        freeBlocks_.erase(next);
    }

    // Roll back the bump pointer if the highest free block reaches it.
    while (!freeBlocks_.empty()) {
        auto last = std::prev(freeBlocks_.end());
        if (static_cast<uint64_t>(last->first) + last->second == nextHeapAddr_) {
            nextHeapAddr_ = last->first;
            freeBlocks_.erase(last);
        } else {
            break;
        }
    }

    return true;
}

uint32_t WardenEmulator::getRegister(int regId) {
    uint32_t value = 0;
    if (uc_) {
        uc_reg_read(uc_, regId, &value);
    }
    return value;
}

void WardenEmulator::setRegister(int regId, uint32_t value) {
    if (uc_) {
        uc_reg_write(uc_, regId, &value);
    }
}

// ============================================================================
// Windows API Implementations
// ============================================================================

uint32_t WardenEmulator::apiVirtualAlloc(WardenEmulator& emu, const std::vector<uint32_t>& args) {
    // VirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect)
    if (args.size() < 4) return 0;

    uint32_t lpAddress = args[0];
    uint32_t dwSize = args[1];
    uint32_t flAllocationType = args[2];
    uint32_t flProtect = args[3];

    {
        char vBuf[128];
        std::snprintf(vBuf, sizeof(vBuf), "WinAPI: VirtualAlloc(0x%X, %u, 0x%X, 0x%X)",
                      lpAddress, dwSize, flAllocationType, flProtect);
        LOG_DEBUG(vBuf);
    }

    // Ignore lpAddress hint for now
    return emu.allocateMemory(dwSize, flProtect);
}

uint32_t WardenEmulator::apiVirtualFree(WardenEmulator& emu, const std::vector<uint32_t>& args) {
    // VirtualFree(lpAddress, dwSize, dwFreeType)
    if (args.size() < 3) return 0;

    uint32_t lpAddress = args[0];

    {
        char vBuf[64];
        std::snprintf(vBuf, sizeof(vBuf), "WinAPI: VirtualFree(0x%X)", lpAddress);
        LOG_DEBUG(vBuf);
    }

    return emu.freeMemory(lpAddress) ? 1 : 0;
}

uint32_t WardenEmulator::apiGetTickCount([[maybe_unused]] WardenEmulator& emu, [[maybe_unused]] const std::vector<uint32_t>& args) {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    uint32_t ticks = static_cast<uint32_t>(ms & 0xFFFFFFFF);

    LOG_DEBUG("WinAPI: GetTickCount() = ", ticks);
    return ticks;
}

uint32_t WardenEmulator::apiGetSystemTimeAsFileTime(WardenEmulator& emu, const std::vector<uint32_t>& args) {
    if (args.empty() || args[0] == 0) return 0;

    auto now = std::chrono::system_clock::now().time_since_epoch();
    uint64_t unix100ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() / 100);
    constexpr uint64_t kFiletimeUnixEpochDelta = 116444736000000000ULL;
    uint64_t filetime = unix100ns + kFiletimeUnixEpochDelta;
    uint32_t low = static_cast<uint32_t>(filetime & 0xFFFFFFFFu);
    uint32_t high = static_cast<uint32_t>(filetime >> 32);

    if (!emu.writeMemory(args[0], &low, 4) ||
        !emu.writeMemory(args[0] + 4, &high, 4)) {
        return 0;
    }
    LOG_DEBUG("WinAPI: GetSystemTimeAsFileTime(0x", std::hex, args[0], std::dec, ")");
    return 1;
}

uint32_t WardenEmulator::apiQueryPerformanceCounter(WardenEmulator& emu, const std::vector<uint32_t>& args) {
    if (args.empty() || args[0] == 0) return 0;

    int64_t ticks = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!emu.writeMemory(args[0], &ticks, sizeof(ticks))) {
        return 0;
    }
    LOG_DEBUG("WinAPI: QueryPerformanceCounter(0x", std::hex, args[0], std::dec, ")");
    return 1;
}

uint32_t WardenEmulator::apiGetModuleHandleA(WardenEmulator& emu, const std::vector<uint32_t>& args) {
    std::string moduleName;
    if (!args.empty() && args[0] != 0) {
        moduleName = emu.readString(args[0], 128);
    }
    LOG_DEBUG("WinAPI: GetModuleHandleA(",
              moduleName.empty() ? "NULL/self" : moduleName,
              ") = 0x", std::hex, emu.getModuleBase(), std::dec);
    return emu.getModuleBase();
}

uint32_t WardenEmulator::apiLoadLibraryA(WardenEmulator& emu, const std::vector<uint32_t>& args) {
    std::string libraryName;
    if (!args.empty() && args[0] != 0) {
        libraryName = emu.readString(args[0], 128);
    }
    LOG_DEBUG("WinAPI: LoadLibraryA(",
              libraryName.empty() ? "?" : libraryName,
              ") = 0x", std::hex, emu.getModuleBase(), std::dec);
    return emu.getModuleBase();
}

uint32_t WardenEmulator::apiFreeLibrary([[maybe_unused]] WardenEmulator& emu, const std::vector<uint32_t>& args) {
    uint32_t module = args.empty() ? 0 : args[0];
    LOG_DEBUG("WinAPI: FreeLibrary(0x", std::hex, module, std::dec, ") = 1");
    return 1;
}

uint32_t WardenEmulator::apiGetProcAddress(WardenEmulator& emu, const std::vector<uint32_t>& args) {
    if (args.size() < 2 || args[1] == 0) return 0;

    std::string procName;
    if (args[1] < 0x10000) {
        procName = "#" + std::to_string(args[1]);
    } else {
        procName = emu.readString(args[1], 128);
    }
    if (procName.empty()) return 0;
    if (procName.rfind("wine_", 0) == 0) {
        LOG_DEBUG("WinAPI: GetProcAddress(", procName, ") -> 0");
        return 0;
    }

    uint32_t resolved = 0;
    for (const auto& [dllName, funcs] : emu.apiAddresses_) {
        auto it = funcs.find(procName);
        if (it != funcs.end()) {
            resolved = it->second;
            break;
        }
    }
    if (resolved == 0) {
        resolved = emu.hookAPI("kernel32.dll", procName,
            [](WardenEmulator&, const std::vector<uint32_t>&) -> uint32_t {
                return 0;
            });
        LOG_WARNING("WinAPI: GetProcAddress auto-stubbed ", procName,
                    " -> 0x", std::hex, resolved, std::dec);
    } else {
        LOG_DEBUG("WinAPI: GetProcAddress(", procName, ") -> 0x",
                  std::hex, resolved, std::dec);
    }
    return resolved;
}

uint32_t WardenEmulator::apiGetSystemInfo(WardenEmulator& emu, const std::vector<uint32_t>& args) {
    if (args.empty() || args[0] == 0) return 0;

    uint8_t info[36] = {};
    auto put32 = [&](size_t off, uint32_t value) {
        std::memcpy(info + off, &value, sizeof(value));
    };
    auto put16 = [&](size_t off, uint16_t value) {
        std::memcpy(info + off, &value, sizeof(value));
    };
    put32(0, 0);              // dwOemId / processor architecture
    put32(4, 0x1000);         // dwPageSize
    put32(8, 0x00010000);     // lpMinimumApplicationAddress
    put32(12, 0x7FFEFFFF);    // lpMaximumApplicationAddress
    put32(16, 0x00000001);    // dwActiveProcessorMask
    put32(20, 1);             // dwNumberOfProcessors
    put32(24, 586);           // dwProcessorType
    put32(28, 0x10000);       // dwAllocationGranularity
    put16(32, 6);             // wProcessorLevel
    put16(34, 0x3A09);        // wProcessorRevision
    emu.writeMemory(args[0], info, sizeof(info));
    LOG_DEBUG("WinAPI: GetSystemInfo(0x", std::hex, args[0], std::dec, ")");
    return 0;
}

uint32_t WardenEmulator::apiGetVersionExA(WardenEmulator& emu, const std::vector<uint32_t>& args) {
    if (args.empty() || args[0] == 0) return 0;

    uint32_t size = 0;
    emu.readMemory(args[0], &size, sizeof(size));
    if (size < 20) size = 20;
    std::vector<uint8_t> info(std::min<uint32_t>(size, 156), 0);
    auto put32 = [&](size_t off, uint32_t value) {
        if (off + sizeof(value) <= info.size()) {
            std::memcpy(info.data() + off, &value, sizeof(value));
        }
    };
    put32(0, size);
    put32(4, 5);      // Windows XP major
    put32(8, 1);      // Windows XP minor
    put32(12, 2600);  // build
    put32(16, 2);     // VER_PLATFORM_WIN32_NT
    emu.writeMemory(args[0], info.data(), info.size());
    LOG_DEBUG("WinAPI: GetVersionExA(0x", std::hex, args[0], std::dec, ") = 1");
    return 1;
}

uint32_t WardenEmulator::apiCreateToolhelp32Snapshot([[maybe_unused]] WardenEmulator& emu,
                                                     const std::vector<uint32_t>& args) {
    uint32_t flags = args.size() > 0 ? args[0] : 0;
    uint32_t pid = args.size() > 1 ? args[1] : 0;
    LOG_DEBUG("WinAPI: CreateToolhelp32Snapshot(flags=0x", std::hex, flags,
              ", pid=", pid, std::dec, ") = INVALID_HANDLE_VALUE");
    return 0xFFFFFFFFu;
}

uint32_t WardenEmulator::apiModule32First([[maybe_unused]] WardenEmulator& emu,
                                          [[maybe_unused]] const std::vector<uint32_t>& args) {
    LOG_DEBUG("WinAPI: Module32First() = 0");
    return 0;
}

uint32_t WardenEmulator::apiModule32Next([[maybe_unused]] WardenEmulator& emu,
                                         [[maybe_unused]] const std::vector<uint32_t>& args) {
    LOG_DEBUG("WinAPI: Module32Next() = 0");
    return 0;
}

uint32_t WardenEmulator::apiTlsAlloc(WardenEmulator& emu, [[maybe_unused]] const std::vector<uint32_t>& args) {
    uint32_t index = emu.nextTlsIndex_++;
    emu.tlsValues_[index] = 0;
    LOG_DEBUG("WinAPI: TlsAlloc() = ", index);
    return index;
}

uint32_t WardenEmulator::apiTlsFree(WardenEmulator& emu, const std::vector<uint32_t>& args) {
    if (args.empty()) return 0;
    emu.tlsValues_.erase(args[0]);
    LOG_DEBUG("WinAPI: TlsFree(", args[0], ")");
    return 1;
}

uint32_t WardenEmulator::apiTlsSetValue(WardenEmulator& emu, const std::vector<uint32_t>& args) {
    if (args.size() < 2) return 0;
    emu.tlsValues_[args[0]] = args[1];
    LOG_DEBUG("WinAPI: TlsSetValue(", args[0], ", 0x", std::hex, args[1], std::dec, ")");
    return 1;
}

uint32_t WardenEmulator::apiTlsGetValue(WardenEmulator& emu, const std::vector<uint32_t>& args) {
    if (args.empty()) return 0;
    auto it = emu.tlsValues_.find(args[0]);
    uint32_t value = (it == emu.tlsValues_.end()) ? 0 : it->second;
    LOG_DEBUG("WinAPI: TlsGetValue(", args[0], ") = 0x", std::hex, value, std::dec);
    return value;
}

uint32_t WardenEmulator::apiAddVectoredExceptionHandler([[maybe_unused]] WardenEmulator& emu,
                                                        const std::vector<uint32_t>& args) {
    uint32_t handler = args.size() >= 2 ? args[1] : 0;
    LOG_DEBUG("WinAPI: AddVectoredExceptionHandler(handler=0x", std::hex, handler, std::dec, ")");
    return 0x7000F000u;
}

uint32_t WardenEmulator::apiRemoveVectoredExceptionHandler([[maybe_unused]] WardenEmulator& emu,
                                                           const std::vector<uint32_t>& args) {
    uint32_t handle = args.empty() ? 0 : args[0];
    LOG_DEBUG("WinAPI: RemoveVectoredExceptionHandler(0x", std::hex, handle, std::dec, ")");
    return 1;
}

uint32_t WardenEmulator::apiSleep([[maybe_unused]] WardenEmulator& emu, const std::vector<uint32_t>& args) {
    if (args.size() < 1) return 0;
    uint32_t dwMilliseconds = args[0];

    LOG_DEBUG("WinAPI: Sleep(", dwMilliseconds, ")");
    // Don't actually sleep in emulator
    return 0;
}

uint32_t WardenEmulator::apiGetCurrentThreadId([[maybe_unused]] WardenEmulator& emu, [[maybe_unused]] const std::vector<uint32_t>& args) {
    LOG_DEBUG("WinAPI: GetCurrentThreadId() = 1234");
    return 1234; // Fake thread ID
}

uint32_t WardenEmulator::apiGetCurrentProcessId([[maybe_unused]] WardenEmulator& emu, [[maybe_unused]] const std::vector<uint32_t>& args) {
    LOG_DEBUG("WinAPI: GetCurrentProcessId() = 5678");
    return 5678; // Fake process ID
}

uint32_t WardenEmulator::apiReadProcessMemory(WardenEmulator& emu, const std::vector<uint32_t>& args) {
    // ReadProcessMemory(hProcess, lpBaseAddress, lpBuffer, nSize, lpNumberOfBytesRead)
    if (args.size() < 5) return 0;

    [[maybe_unused]] uint32_t hProcess = args[0];
    uint32_t lpBaseAddress = args[1];
    uint32_t lpBuffer = args[2];
    uint32_t nSize = args[3];
    uint32_t lpNumberOfBytesRead = args[4];

    {
        char rBuf[64];
        std::snprintf(rBuf, sizeof(rBuf), "WinAPI: ReadProcessMemory(0x%X, %u bytes)", lpBaseAddress, nSize);
        LOG_DEBUG(rBuf);
    }

    // Read from emulated memory and write to buffer
    std::vector<uint8_t> data(nSize);
    if (!emu.readMemory(lpBaseAddress, data.data(), nSize)) {
        return 0; // Failure
    }

    if (!emu.writeMemory(lpBuffer, data.data(), nSize)) {
        return 0; // Failure
    }

    if (lpNumberOfBytesRead != 0) {
        emu.writeMemory(lpNumberOfBytesRead, &nSize, 4);
    }

    return 1; // Success
}

// ============================================================================
// Unicorn Callbacks
// ============================================================================

void WardenEmulator::hookCode(uc_engine* uc, uint64_t address, [[maybe_unused]] uint32_t size, void* userData) {
    auto* self = static_cast<WardenEmulator*>(userData);
    if (!self) return;
    self->lastCodeAddress_ = static_cast<uint32_t>(address);
    self->lastCodeSize_ = size;

    auto it = self->apiHandlers_.find(static_cast<uint32_t>(address));
    if (it == self->apiHandlers_.end()) return; // not an API stub — trace disabled to avoid spam

    const ApiHookEntry& entry = it->second;

    // Read stack: [ESP+0] = return address, [ESP+4..] = stdcall args
    uint32_t esp = 0;
    uc_reg_read(uc, UC_X86_REG_ESP, &esp);

    uint32_t retAddr = 0;
    uc_mem_read(uc, esp, &retAddr, 4);

    std::vector<uint32_t> args(static_cast<size_t>(entry.argCount));
    for (int i = 0; i < entry.argCount; ++i) {
        uint32_t val = 0;
        uc_mem_read(uc, esp + 4 + static_cast<uint32_t>(i) * 4, &val, 4);
        args[static_cast<size_t>(i)] = val;
    }

    {
        std::ostringstream oss;
        oss << "WardenEmulator: API stub " << entry.name
            << " addr=0x" << std::hex << static_cast<uint32_t>(address)
            << " ret=0x" << retAddr
            << " esp=0x" << esp
            << " args=" << std::dec << entry.argCount;
        for (int i = 0; i < entry.argCount; ++i) {
            oss << " arg" << i << "=0x" << std::hex << args[static_cast<size_t>(i)];
        }
        LOG_WARNING(oss.str());
    }

    // Dispatch to the C++ handler
    uint32_t retVal = 0;
    if (entry.handler) {
        retVal = entry.handler(*self, args);
    }

    // Simulate stdcall epilogue: pop return address + args
    uint32_t newEsp = esp + 4 + static_cast<uint32_t>(entry.argCount) * 4;
    uc_reg_write(uc, UC_X86_REG_EAX, &retVal);
    uc_reg_write(uc, UC_X86_REG_ESP, &newEsp);
    uc_reg_write(uc, UC_X86_REG_EIP, &retAddr);
    LOG_WARNING("WardenEmulator: API stub returned EAX=0x", std::hex, retVal,
                " newESP=0x", newEsp, " nextEIP=0x", retAddr, std::dec);
}

bool WardenEmulator::hookMemInvalid(uc_engine* uc, int type, uint64_t address, int size, [[maybe_unused]] int64_t value, void* userData) {

    const char* typeStr = "UNKNOWN";
    switch (type) {
        case UC_MEM_READ_UNMAPPED: typeStr = "READ_UNMAPPED"; break;
        case UC_MEM_WRITE_UNMAPPED: typeStr = "WRITE_UNMAPPED"; break;
        case UC_MEM_FETCH_UNMAPPED: typeStr = "FETCH_UNMAPPED"; break;
        case UC_MEM_READ_PROT: typeStr = "READ_PROT"; break;
        case UC_MEM_WRITE_PROT: typeStr = "WRITE_PROT"; break;
        case UC_MEM_FETCH_PROT: typeStr = "FETCH_PROT"; break;
    }

    {
        char mBuf[128];
        std::snprintf(mBuf, sizeof(mBuf), "WardenEmulator: Invalid memory access: %s at 0x%llX (size=%d)",
                      typeStr, static_cast<unsigned long long>(address), size);
        LOG_ERROR(mBuf);
    }

    auto* self = static_cast<WardenEmulator*>(userData);
    uint32_t eip = 0, esp = 0, ecx = 0, eax = 0;
    uc_reg_read(uc, UC_X86_REG_EIP, &eip);
    uc_reg_read(uc, UC_X86_REG_ESP, &esp);
    uc_reg_read(uc, UC_X86_REG_ECX, &ecx);
    uc_reg_read(uc, UC_X86_REG_EAX, &eax);
    LOG_ERROR("WardenEmulator: invalid access regs EIP=0x", std::hex, eip,
              " ESP=0x", esp, " ECX=0x", ecx, " EAX=0x", eax,
              " last=0x", (self ? self->lastCodeAddress_ : 0),
              "+", (self ? self->lastCodeSize_ : 0), std::dec);

    uint8_t stack[32] = {};
    if (uc_mem_read(uc, esp, stack, sizeof(stack)) == UC_ERR_OK) {
        std::ostringstream oss;
        oss << "WardenEmulator: stack[ESP..+32]=";
        for (uint8_t b : stack) {
            oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b) << ' ';
        }
        LOG_ERROR(oss.str());
    } else {
        LOG_ERROR("WardenEmulator: could not read stack at ESP=0x", std::hex, esp, std::dec);
    }

    auto logBytesAt = [&](const char* label, uint32_t start) {
        if (!self || !self->isRangeMapped(start, 1)) return;
        uint8_t bytes[16] = {};
        if (uc_mem_read(uc, start, bytes, sizeof(bytes)) != UC_ERR_OK) return;
        std::ostringstream oss;
        oss << "WardenEmulator: " << label << " bytes @0x" << std::hex << start << "=";
        for (uint8_t b : bytes) {
            oss << std::setw(2) << std::setfill('0') << static_cast<int>(b) << ' ';
        }
        LOG_ERROR(oss.str());
    };
    if (self && self->lastCodeAddress_ != 0) {
        logBytesAt("last", self->lastCodeAddress_);
    }
    if (address <= 0xFFFFFFFFull) {
        logBytesAt("fault", static_cast<uint32_t>(address));
    }

    if ((type == UC_MEM_FETCH_PROT || type == UC_MEM_FETCH_UNMAPPED) && address == 0) {
        uint32_t retAddr = 0;
        if (uc_mem_read(uc, esp, &retAddr, 4) == UC_ERR_OK &&
            self && self->isRangeMapped(retAddr, 1)) {
            uint32_t newEsp = esp + 4;
            uint32_t eaxZero = 0;
            uc_reg_write(uc, UC_X86_REG_EIP, &retAddr);
            uc_reg_write(uc, UC_X86_REG_ESP, &newEsp);
            uc_reg_write(uc, UC_X86_REG_EAX, &eaxZero);
            LOG_WARNING("WardenEmulator: recovered NULL code call as no-op return to 0x",
                        std::hex, retAddr, " newESP=0x", newEsp, std::dec);
            return true;
        }
    }
    return false;
}

#else // !HAVE_UNICORN
// Stub implementations — Unicorn Engine not available on this platform.
WardenEmulator::WardenEmulator()
    : uc_(nullptr), moduleBase_(0), moduleSize_(0)
    , stackBase_(0), stackSize_(0)
    , heapBase_(0), heapSize_(0)
    , apiStubBase_(0), nextApiStubAddr_(0), apiCodeHookRegistered_(false)
    , nextHeapAddr_(0) {}
WardenEmulator::~WardenEmulator() {}
bool WardenEmulator::initialize(const void*, size_t, uint32_t) { return false; }
bool WardenEmulator::syncModuleMemory(const void*, size_t) { return false; }
uint32_t WardenEmulator::hookAPI(const std::string&, const std::string&,
    std::function<uint32_t(WardenEmulator&, const std::vector<uint32_t>&)>) { return 0; }
uint32_t WardenEmulator::hookFunction(const std::string&, int,
    std::function<uint32_t(WardenEmulator&, const std::vector<uint32_t>&)>) { return 0; }
uint32_t WardenEmulator::callFunction(uint32_t, const std::vector<uint32_t>&) { return 0; }
uint32_t WardenEmulator::callThiscall(uint32_t, uint32_t, const std::vector<uint32_t>&) { return 0; }
bool WardenEmulator::readMemory(uint32_t, void*, size_t) { return false; }
bool WardenEmulator::writeMemory(uint32_t, const void*, size_t) { return false; }
std::string WardenEmulator::readString(uint32_t, size_t) { return {}; }
uint32_t WardenEmulator::allocateMemory(size_t, uint32_t) { return 0; }
bool WardenEmulator::freeMemory(uint32_t) { return false; }
uint32_t WardenEmulator::getRegister(int) { return 0; }
void WardenEmulator::setRegister(int, uint32_t) {}
bool WardenEmulator::isRangeMapped(uint32_t, size_t) const { return false; }
void WardenEmulator::setupCommonAPIHooks() {}
uint32_t WardenEmulator::getAPIAddress(const std::string&, const std::string&) const { return 0; }
uint32_t WardenEmulator::writeData(const void*, size_t) { return 0; }
std::vector<uint8_t> WardenEmulator::readData(uint32_t, size_t) { return {}; }
void WardenEmulator::hookCode(uc_engine*, uint64_t, uint32_t, void*) {}
bool WardenEmulator::hookMemInvalid(uc_engine*, int, uint64_t, int, int64_t, void*) { return false; }
#endif // HAVE_UNICORN

} // namespace game
} // namespace wowee
