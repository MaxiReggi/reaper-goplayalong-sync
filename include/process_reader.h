#pragma once

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <winver.h>

#include <format>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

namespace tnt {

// Reads memory from an external Windows process by navigating pointer chains.
// Used to extract playback state from applications that have no public API.
class ProcessReader final
{
public:
    // Opens the target process and locates the given module within it.
    // Throws std::runtime_error if the process or module is not found.
    ProcessReader(const std::wstring& process_name, const std::wstring& module_name)
    {
        m_process_id = GetProcessID(process_name);
        m_process_path = GetProcessPath(m_process_id);
        m_module_base_address = GetModuleBaseAddress(m_process_id, module_name);

        m_process_handle = OpenProcess(PROCESS_VM_READ, FALSE, m_process_id);
        if (m_process_handle == nullptr)
        {
            throw std::runtime_error(std::format("Failed to open process handle (error {:d}).\n", GetLastError()));
        }
    }

    ~ProcessReader()
    {
        if (m_process_handle != nullptr)
        {
            CloseHandle(m_process_handle);
        }
    }

    // Returns the file version of the target process (e.g. L"4.0.1.0")
    std::wstring GetProcessVersion() const
    {
        DWORD dummy = 0;
        const DWORD size = GetFileVersionInfoSizeW(m_process_path.c_str(), &dummy);
        if (size == 0)
        {
            throw std::runtime_error("Failed to get version info size.\n");
        }

        std::vector<BYTE> buffer(size);
        if (!GetFileVersionInfoW(m_process_path.c_str(), 0, size, buffer.data()))
        {
            throw std::runtime_error("Failed to read version info.\n");
        }

        VS_FIXEDFILEINFO* info = nullptr;
        UINT info_size = 0;
        if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&info), &info_size))
        {
            throw std::runtime_error("Failed to query version value.\n");
        }

        const int major = HIWORD(info->dwFileVersionMS);
        const int minor = LOWORD(info->dwFileVersionMS);
        const int patch = HIWORD(info->dwFileVersionLS);
        const int build = LOWORD(info->dwFileVersionLS);

        return std::format(L"{}.{}.{}.{}", major, minor, patch, build);
    }

    // Reads a value of type T by following a multi-level pointer chain.
    //
    // Starting from (module_base + module_offset), each offset in the list
    // is added to the current address and then dereferenced as a pointer,
    // except for the last offset which is the final address offset where
    // the value T is read.
    //
    // This matches the pointer chain format exported by CheatEngine.
    template <typename T>
    T ReadMemoryAddress(const int module_offset, const std::initializer_list<int>& offsets) const
    {
        const uintptr_t base = m_module_base_address + static_cast<uintptr_t>(module_offset);
        const uintptr_t address = ReadPointer(base, offsets);

        T value{};
        SIZE_T bytes_read = 0;
        if (!::ReadProcessMemory(m_process_handle, reinterpret_cast<LPCVOID>(address), &value, sizeof(T), &bytes_read))
        {
            const DWORD error = GetLastError();
            if (error == ERROR_ACCESS_DENIED)
            {
                throw std::runtime_error(std::format("ReadProcessMemory: access denied at 0x{:X}.\n", address));
            }
            else if (error == ERROR_INVALID_PARAMETER)
            {
                throw std::runtime_error(std::format("ReadProcessMemory: invalid parameter at 0x{:X}.\n", address));
            }
            throw std::runtime_error(std::format("ReadProcessMemory: partial copy at 0x{:X} ({} of {} bytes read).\n",
                address, bytes_read, sizeof(T)));
        }

        return value;
    }

private:
    DWORD GetProcessID(const std::wstring& process_name) const
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            throw std::runtime_error("Failed to create process snapshot.\n");
        }

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);

        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                if (process_name == entry.szExeFile)
                {
                    CloseHandle(snapshot);
                    return entry.th32ProcessID;
                }
            } while (Process32NextW(snapshot, &entry));
        }

        CloseHandle(snapshot);
        throw std::runtime_error(std::format("Process not found: '{}'.\nMake sure GoPlayAlong is running.\n",
            std::string(process_name.begin(), process_name.end())));
    }

    std::wstring GetProcessPath(const DWORD process_id) const
    {
        HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
        if (handle == nullptr)
        {
            throw std::runtime_error("Failed to open process for path query.\n");
        }

        wchar_t path[MAX_PATH]{};
        DWORD size = MAX_PATH;
        if (!QueryFullProcessImageNameW(handle, 0, path, &size))
        {
            CloseHandle(handle);
            throw std::runtime_error("Failed to query process image name.\n");
        }

        CloseHandle(handle);
        return std::wstring(path, size);
    }

    uintptr_t GetModuleBaseAddress(const DWORD process_id, const std::wstring& module_name) const
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            throw std::runtime_error("Failed to create module snapshot.\n");
        }

        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);

        if (Module32FirstW(snapshot, &entry))
        {
            do
            {
                if (module_name == entry.szModule)
                {
                    CloseHandle(snapshot);
                    return reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                }
            } while (Module32NextW(snapshot, &entry));
        }

        CloseHandle(snapshot);
        throw std::runtime_error(std::format("Module not found: '{}'.\n",
            std::string(module_name.begin(), module_name.end())));
    }

    // Follows a pointer chain through process memory.
    // All offsets except the last one are followed as pointer dereferences.
    // The last offset is added to the final pointer to get the value address.
    uintptr_t ReadPointer(const uintptr_t base, const std::initializer_list<int>& offsets) const
    {
        uintptr_t address = base;

        auto it = offsets.begin();
        const auto end = offsets.end();

        while (it != end)
        {
            const int offset = *it;
            ++it;

            address += static_cast<uintptr_t>(offset);

            // Last offset: this IS the value address, don't dereference
            if (it == end)
            {
                break;
            }

            // Follow pointer
            uintptr_t pointer = 0;
            SIZE_T bytes_read = 0;
            if (!::ReadProcessMemory(m_process_handle, reinterpret_cast<LPCVOID>(address), &pointer, sizeof(pointer), &bytes_read))
            {
                throw std::runtime_error(std::format("ReadPointer: failed at address 0x{:X} (error {:d}).\n",
                    address, GetLastError()));
            }
            address = pointer;
        }

        return address;
    }

    DWORD m_process_id = 0;
    std::wstring m_process_path;
    uintptr_t m_module_base_address = 0;
    HANDLE m_process_handle = nullptr;
};

}
