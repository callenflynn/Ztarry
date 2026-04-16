#define NOMINMAX

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <urlmon.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr const wchar_t *kDownloadUrl =
	L"https://github.com/callenflynn/Ztarry/releases/latest/download/Ztarry.exe";

std::wstring to_lower(std::wstring value) {
	std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
		return static_cast<wchar_t>(std::towlower(ch));
	});
	return value;
}

std::wstring trim_trailing_slashes(std::wstring value) {
	while (!value.empty() && (value.back() == L'\\' || value.back() == L'/')) {
		value.pop_back();
	}
	return value;
}

bool is_elevated() {
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		return false;
	}

	TOKEN_ELEVATION elevation{};
	DWORD size = 0;
	const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
	CloseHandle(token);
	return ok && elevation.TokenIsElevated != 0;
}

std::wstring get_program_files_dir() {
	wchar_t buffer[MAX_PATH] = {};
	const DWORD len = GetEnvironmentVariableW(L"ProgramFiles", buffer, MAX_PATH);
	if (len == 0 || len >= MAX_PATH) {
		return L"C:\\Program Files";
	}
	return std::wstring(buffer);
}

std::wstring get_local_appdata_dir() {
	PWSTR path = nullptr;
	if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path) == S_OK && path != nullptr) {
		std::wstring out(path);
		CoTaskMemFree(path);
		return out;
	}

	wchar_t buffer[MAX_PATH] = {};
	const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
	if (len == 0 || len >= MAX_PATH) {
		return L"C:\\Users\\Public\\AppData\\Local";
	}
	return std::wstring(buffer);
}

std::wstring get_install_dir() {
	return get_program_files_dir() + L"\\Ztarry";
}

DWORD relaunch_as_admin_and_wait() {
	wchar_t exePath[MAX_PATH] = {};
	const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
	if (len == 0 || len >= MAX_PATH) {
		return 11;
	}

	SHELLEXECUTEINFOW sei{};
	sei.cbSize = sizeof(sei);
	sei.fMask = SEE_MASK_NOCLOSEPROCESS;
	sei.hwnd = nullptr;
	sei.lpVerb = L"runas";
	sei.lpFile = exePath;
	sei.lpParameters = L"";
	sei.lpDirectory = nullptr;
	sei.nShow = SW_HIDE;

	if (!ShellExecuteExW(&sei)) {
		const DWORD err = GetLastError();
		if (err == ERROR_CANCELLED) {
			return 12;
		}
		return 13;
	}

	if (sei.hProcess == nullptr) {
		return 14;
	}

	WaitForSingleObject(sei.hProcess, INFINITE);

	DWORD exitCode = 15;
	GetExitCodeProcess(sei.hProcess, &exitCode);
	CloseHandle(sei.hProcess);
	return exitCode;
}

bool get_temp_file_path(std::wstring &pathOut) {
	wchar_t tempDir[MAX_PATH] = {};
	const DWORD len = GetTempPathW(MAX_PATH, tempDir);
	if (len == 0 || len >= MAX_PATH) {
		return false;
	}

	wchar_t tempFile[MAX_PATH] = {};
	if (!GetTempFileNameW(tempDir, L"zty", 0, tempFile)) {
		return false;
	}

	pathOut = tempFile;
	return true;
}

bool download_file(const std::wstring &url, const std::wstring &destPath) {
	const HRESULT hr = URLDownloadToFileW(nullptr, url.c_str(), destPath.c_str(), 0, nullptr);
	return SUCCEEDED(hr);
}

bool install_binary(const std::wstring &downloadPath, const std::wstring &installDir) {
	std::error_code ec;
	std::filesystem::create_directories(installDir, ec);
	if (ec) {
		return false;
	}

	const std::wstring targetPath = installDir + L"\\Ztarry.exe";
	if (MoveFileExW(downloadPath.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
		return true;
	}

	if (!CopyFileW(downloadPath.c_str(), targetPath.c_str(), FALSE)) {
		return false;
	}

	DeleteFileW(downloadPath.c_str());
	return true;
}

bool path_contains_dir(const std::wstring &pathValue, const std::wstring &installDir) {
	const std::wstring target = to_lower(trim_trailing_slashes(installDir));
	size_t start = 0;
	while (start <= pathValue.size()) {
		size_t end = pathValue.find(L';', start);
		if (end == std::wstring::npos) {
			end = pathValue.size();
		}

		std::wstring part = pathValue.substr(start, end - start);
		part = to_lower(trim_trailing_slashes(part));
		if (!part.empty() && part == target) {
			return true;
		}

		if (end == pathValue.size()) {
			break;
		}
		start = end + 1;
	}
	return false;
}

bool add_to_path_registry(HKEY root, const wchar_t *subKey, const std::wstring &installDir) {
	HKEY key = nullptr;
	if (RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
		return false;
	}

	DWORD type = REG_EXPAND_SZ;
	DWORD sizeBytes = 0;
	LONG query = RegQueryValueExW(key, L"Path", nullptr, &type, nullptr, &sizeBytes);

	std::wstring current;
	if (query == ERROR_SUCCESS && sizeBytes > sizeof(wchar_t)) {
		std::vector<wchar_t> buffer(sizeBytes / sizeof(wchar_t));
		if (RegQueryValueExW(key, L"Path", nullptr, &type, reinterpret_cast<LPBYTE>(buffer.data()), &sizeBytes) == ERROR_SUCCESS) {
			current.assign(buffer.data());
		}
	} else if (query != ERROR_SUCCESS && query != ERROR_FILE_NOT_FOUND) {
		RegCloseKey(key);
		return false;
	}

	if (path_contains_dir(current, installDir)) {
		RegCloseKey(key);
		return true;
	}

	std::wstring updated = current;
	if (!updated.empty() && updated.back() != L';') {
		updated.push_back(L';');
	}
	updated += installDir;

	if (type != REG_SZ && type != REG_EXPAND_SZ) {
		type = REG_EXPAND_SZ;
	}

	const DWORD bytes = static_cast<DWORD>((updated.size() + 1) * sizeof(wchar_t));
	const LONG setResult = RegSetValueExW(
		key,
		L"Path",
		0,
		type,
		reinterpret_cast<const BYTE *>(updated.c_str()),
		bytes);

	RegCloseKey(key);
	return setResult == ERROR_SUCCESS;
}

void broadcast_env_change() {
	DWORD_PTR result = 0;
	SendMessageTimeoutW(
		HWND_BROADCAST,
		WM_SETTINGCHANGE,
		0,
		reinterpret_cast<LPARAM>(L"Environment"),
		SMTO_ABORTIFHUNG,
		5000,
		&result);
}

bool add_install_dir_to_path(const std::wstring &installDir) {
	const bool ok = add_to_path_registry(
		HKEY_LOCAL_MACHINE,
		L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
		installDir);

	if (ok) {
		broadcast_env_change();
	}
	return ok;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
	if (!is_elevated()) {
		return static_cast<int>(relaunch_as_admin_and_wait());
	}

	const std::wstring installDir = get_install_dir();

	std::wstring tempPath;
	if (!get_temp_file_path(tempPath)) {
		return 10;
	}

	if (!download_file(kDownloadUrl, tempPath)) {
		DeleteFileW(tempPath.c_str());
		return 20;
	}

	if (!install_binary(tempPath, installDir)) {
		DeleteFileW(tempPath.c_str());
		return 30;
	}

	if (!add_install_dir_to_path(installDir)) {
		return 40;
	}

	return 0;
}
