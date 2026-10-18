// CoJ Gunslinger VR installer and launcher for Windows (same logic as install-linux.sh).
//   cojvr-setup.exe               install or update: copy of the game next to it (hard links), mod in the copy,
//                                 "Call of Juarez Gunslinger VR" in the Steam library; the original game is never modified
//   cojvr-setup.exe --uninstall   remove the copy and the Steam entry
//   cojvr-setup.exe --launch      (the Steam entry) starts the game in the copy as app 204450
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <wctype.h>
#include <string>
#include <vector>
#include <utility>

using std::string;
using std::wstring;

static const char* APP_NAME = "Call of Juarez Gunslinger VR";
static const wchar_t* COPY_DIR = L"CoJ Gunslinger VR";

static wstring W(const string& s) {
	wstring w(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0), 0);
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], (int)w.size());
	w.pop_back();
	return w;
}
static string U(const wstring& w) {
	string s(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, NULL, 0, NULL, NULL), 0);
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], (int)s.size(), NULL, NULL);
	s.pop_back();
	return s;
}
static bool is_file(const wstring& p) {
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool is_dir(const wstring& p) {
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static string read_file(const wstring& p) {
	string s;
	FILE* f = _wfopen(p.c_str(), L"rb");
	if (!f) return s;
	char buf[65536];
	for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) s.append(buf, n);
	fclose(f);
	return s;
}
static bool write_file(const wstring& p, const string& s) {
	FILE* f = _wfopen(p.c_str(), L"wb");
	if (!f) return false;
	bool ok = fwrite(s.data(), 1, s.size(), f) == s.size();
	return fclose(f) == 0 && ok;
}
static void die(const char* msg) {
	printf("\nERROR: %s\n\nPress Enter to close.", msg);
	getchar();
	ExitProcess(1);
}

// --- Steam folders ---
static wstring steam_root() {
	wchar_t buf[MAX_PATH];
	DWORD n = sizeof buf;
	if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, NULL, buf, &n)) die("Steam not found.");
	wstring r = buf;
	for (auto& c : r)
		if (c == L'/') c = L'\\';
	return r;
}
static std::vector<wstring> vdf_values(const string& text, const string& key) {  // all "key" values in a text VDF ("key"  "value")
	std::vector<wstring> out;
	string pat = "\"" + key + "\"";
	for (size_t i = 0; (i = text.find(pat, i)) != string::npos; i += pat.size()) {
		size_t a = text.find('"', i + pat.size()), b = a == string::npos ? a : text.find('"', a + 1);
		while (b != string::npos && text[b - 1] == '\\' && text[b - 2] != '\\') b = text.find('"', b + 1);
		if (b == string::npos) break;
		string v;
		for (size_t k = a + 1; k < b; k++) v += (text[k] == '\\' && text[k + 1] == '\\') ? text[k++] : text[k];
		out.push_back(W(v));
	}
	return out;
}
static std::vector<wstring> libraries(const wstring& root) {
	std::vector<wstring> libs{root};
	for (auto& p : vdf_values(read_file(root + L"\\steamapps\\libraryfolders.vdf"), "path")) {
		bool dup = false;
		for (auto& l : libs) dup |= !_wcsicmp(l.c_str(), p.c_str());
		if (!dup && is_dir(p + L"\\steamapps")) libs.push_back(p);
	}
	return libs;
}
static bool find_game(const std::vector<wstring>& libs, wstring& lib, wstring& game) {
	for (auto& l : libs) {
		string acf = read_file(l + L"\\steamapps\\appmanifest_204450.acf");
		if (acf.empty()) continue;
		auto dir = vdf_values(acf, "installdir");
		game = l + L"\\steamapps\\common\\" + (dir.empty() ? L"CoJ Gunslinger" : dir[0]);
		lib = l;
		if (is_file(game + L"\\CoJGunslinger.exe")) return true;
	}
	return false;
}
static bool steam_running() {
	HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	PROCESSENTRY32W e = {};
	e.dwSize = sizeof e;
	bool found = false;
	for (BOOL ok = Process32FirstW(s, &e); ok && !found; ok = Process32NextW(s, &e)) found = !_wcsicmp(e.szExeFile, L"steam.exe");
	CloseHandle(s);
	return found;
}
static void wait_steam_closed() {
	if (GetEnvironmentVariableW(L"COJVR_TEST", NULL, 0)) return;  // testes pelo Proton: lá sempre roda um steam.exe próprio
	while (steam_running()) {
		printf("\nSteam is running. Close it (Steam menu > Exit), then press Enter... ");
		getchar();
		Sleep(2000);
	}
}

// --- shortcuts.vdf (binary VDF: 0x00 map, 0x01 string, 0x02 int32, 0x08 end), kept in file order ---
struct Node {
	int type = 0;  // 0 map, 1 string, 2 int
	string s;
	int i = 0;
	std::vector<std::pair<string, Node>> kids;
	Node* get(const string& k) {
		for (auto& kv : kids)
			if (!_stricmp(kv.first.c_str(), k.c_str())) return &kv.second;
		return NULL;
	}
};
static bool vdf_parse(const string& b, size_t& i, Node& n) {
	while (i < b.size()) {
		int t = (unsigned char)b[i++];
		if (t == 8) return true;
		size_t z = b.find('\0', i);
		if (z == string::npos) return false;
		Node c;
		c.type = t;
		string k = b.substr(i, z - i);
		i = z + 1;
		if (t == 0) {
			if (!vdf_parse(b, i, c)) return false;
		} else if (t == 1) {
			z = b.find('\0', i);
			if (z == string::npos) return false;
			c.s = b.substr(i, z - i), i = z + 1;
		} else if (t == 2 && i + 4 <= b.size()) {
			memcpy(&c.i, &b[i], 4), i += 4;
		} else
			return false;
		n.kids.push_back({k, c});
	}
	return false;
}
static void vdf_dump(const Node& n, string& out) {
	for (auto& kv : n.kids) {
		out += (char)kv.second.type, out += kv.first, out += '\0';
		if (kv.second.type == 0) vdf_dump(kv.second, out);
		else if (kv.second.type == 1) out += kv.second.s, out += '\0';
		else out.append((const char*)&kv.second.i, 4);
	}
	out += '\x08';
}
static unsigned crc32(const string& s) {
	unsigned c = 0xFFFFFFFF;
	for (unsigned char ch : s) {
		c ^= ch;
		for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320 & (0u - (c & 1)));
	}
	return ~c;
}
static Node str_node(const string& s) {
	Node n;
	n.type = 1, n.s = s;
	return n;
}
static Node int_node(int v) {
	Node n;
	n.type = 2, n.i = v;
	return n;
}
static int set_shortcut(const wstring& root, const wstring& exe_path, const wstring& start_dir) {
	int users = 0;
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((root + L"\\userdata\\*").c_str(), &fd);
	for (BOOL ok = h != INVALID_HANDLE_VALUE; ok; ok = FindNextFileW(h, &fd)) {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !iswdigit(fd.cFileName[0]) || !wcscmp(fd.cFileName, L"0")) continue;
		wstring cfg = root + L"\\userdata\\" + fd.cFileName + L"\\config";
		CreateDirectoryW(cfg.c_str(), NULL);
		wstring path = cfg + L"\\shortcuts.vdf";
		string b = read_file(path);
		Node top;
		size_t i = 0;
		if (!b.empty() && !vdf_parse(b, i, top)) die("could not read Steam's shortcuts.vdf; nothing was changed.");
		if (!top.get("shortcuts")) top.kids.push_back({"shortcuts", Node()});
		Node& sc = *top.get("shortcuts");
		Node keep;
		for (auto& kv : sc.kids) {
			Node* name = kv.second.get("appname");
			if (!(name && name->s == APP_NAME)) keep.kids.push_back(kv);
		}
		if (!exe_path.empty()) {
			string exe = "\"" + U(exe_path) + "\"";
			Node e;
			e.kids = {{"appid", int_node((int)(crc32(exe + APP_NAME) | 0x80000000))},
			          {"appname", str_node(APP_NAME)},
			          {"Exe", str_node(exe)},
			          {"StartDir", str_node("\"" + U(start_dir) + "\"")},
			          {"icon", str_node(U(start_dir) + "\\CoJGunslinger.exe")},
			          {"ShortcutPath", str_node("")},
			          {"LaunchOptions", str_node("--launch")},
			          {"IsHidden", int_node(0)},
			          {"AllowDesktopConfig", int_node(1)},
			          {"AllowOverlay", int_node(0)},  // com o overlay da Steam, o fim do loading de fase trava no Linux
			          {"OpenVR", int_node(1)},
			          {"Devkit", int_node(0)},
			          {"DevkitGameID", str_node("")},
			          {"DevkitOverrideAppID", int_node(0)},
			          {"LastPlayTime", int_node(0)},
			          {"FlatpakAppID", str_node("")},
			          {"sortas", str_node("")},
			          {"tags", Node()}};
			keep.kids.push_back({"", e});
		}
		for (size_t k = 0; k < keep.kids.size(); k++) keep.kids[k].first = std::to_string(k);
		sc.kids = keep.kids;
		string out;
		vdf_dump(top, out);
		if (!b.empty()) CopyFileW(path.c_str(), (path + L".cojvr-backup").c_str(), FALSE);
		if (!write_file(path, out)) die("could not write Steam's shortcuts.vdf.");
		users++;
	}
	if (h != INVALID_HANDLE_VALUE) FindClose(h);
	return users;
}

// --- files ---
static void put(const wstring& src, const wstring& dst) {  // never writes into an existing file (it may be a hard link)
	DeleteFileW(dst.c_str());
	if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) die(("could not copy " + U(src)).c_str());
}
static void copy_tree(const wstring& src, const wstring& dst, bool link, bool top, int& linked, int& copied) {
	CreateDirectoryW(dst.c_str(), NULL);
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
	for (BOOL ok = h != INVALID_HANDLE_VALUE; ok; ok = FindNextFileW(h, &fd)) {
		wstring n = fd.cFileName, s = src + L"\\" + n, d = dst + L"\\" + n;
		if (n == L"." || n == L"..") continue;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if (!(link && top && !_wcsicmp(n.c_str(), L"cojvr"))) copy_tree(s, d, link, false, linked, copied);
		} else if (link) {  // game -> copy: skip mod files, keep what already exists
			if (top && (!_wcsicmp(n.c_str(), L"d3d9.dll") || !_wcsicmp(n.c_str(), L"steam_appid.txt"))) continue;
			if (GetFileAttributesW(d.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
			if (CreateHardLinkW(d.c_str(), s.c_str(), NULL)) linked++;
			else if (CopyFileW(s.c_str(), d.c_str(), TRUE)) copied++;
			else die(("could not copy " + U(s)).c_str());
		} else if (!(_wcsicmp(n.c_str(), L"cojvr.ini") == 0 && is_file(d))) {  // mod -> copy: keep the player's options
			put(s, d);
		}
	}
	if (h != INVALID_HANDLE_VALUE) FindClose(h);
}
static void remove_tree(const wstring& p) {
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((p + L"\\*").c_str(), &fd);
	for (BOOL ok = h != INVALID_HANDLE_VALUE; ok; ok = FindNextFileW(h, &fd)) {
		wstring n = fd.cFileName, s = p + L"\\" + n;
		if (n == L"." || n == L"..") continue;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(s);
		else SetFileAttributesW(s.c_str(), FILE_ATTRIBUTE_NORMAL), DeleteFileW(s.c_str());
	}
	if (h != INVALID_HANDLE_VALUE) FindClose(h);
	RemoveDirectoryW(p.c_str());
}

static wstring self_path() {
	wchar_t buf[MAX_PATH];
	GetModuleFileNameW(NULL, buf, MAX_PATH);
	return buf;
}
static wstring dir_of(const wstring& p) { return p.substr(0, p.find_last_of(L"\\/")); }

static int launch() {
	wstring dir = dir_of(self_path()), exe = dir + L"\\CoJGunslinger.exe";
	SetEnvironmentVariableW(L"SteamAppId", L"204450");
	SetEnvironmentVariableW(L"SteamGameId", L"204450");
	STARTUPINFOW si = {};
	si.cb = sizeof si;
	PROCESS_INFORMATION pi;
	if (!CreateProcessW(exe.c_str(), NULL, NULL, NULL, FALSE, 0, NULL, dir.c_str(), &si, &pi)) {
		MessageBoxA(NULL, "Could not start CoJGunslinger.exe from the VR copy. Run cojvr-setup.exe again.", APP_NAME, MB_ICONERROR);
		return 1;
	}
	WaitForSingleObject(pi.hProcess, INFINITE);  // Steam shows the game as running until it closes
	CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
	return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmd, int) {
	if (wcsstr(cmd, L"--launch")) return launch();
	AllocConsole();
	freopen("CONOUT$", "w", stdout), freopen("CONIN$", "r", stdin);
	SetConsoleOutputCP(CP_UTF8);
	wstring root = steam_root(), lib, game, here = dir_of(self_path());
	auto libs = libraries(root);
	if (!find_game(libs, lib, game)) die("Call of Juarez Gunslinger is not installed in any Steam library.");
	wstring dest = lib + L"\\steamapps\\common\\" + COPY_DIR, marker = dest + L"\\cojvr\\.vr_copy";
	printf("Steam: %s\nGame:  %s\n", U(root).c_str(), U(game).c_str());
	if (wcsstr(cmd, L"--uninstall")) {
		wait_steam_closed();
		set_shortcut(root, L"", L"");
		if (is_file(marker)) remove_tree(dest), printf("Removed %s.\n", U(dest).c_str());
		printf("\"%s\" was removed from Steam. The normal game is unchanged.\n\nPress Enter to close.", APP_NAME);
		getchar();
		return 0;
	}
	for (auto f : {L"\\d3d9.dll", L"\\cojvr\\dxvk_d3d9.dll", L"\\cojvr\\openvr_api.dll"})
		if (!is_file(here + f)) die("files are missing next to the installer. Extract the whole zip and run it from there.");
	if (is_dir(dest) && !is_file(marker)) die("the folder \"CoJ Gunslinger VR\" already exists and was not made by this installer; move it away first.");
	printf("Copying the game to %s (hard links, almost no extra disk)...\n", U(dest).c_str());
	int linked = 0, copied = 0;
	copy_tree(game, dest, true, true, linked, copied);
	printf("  %d files linked, %d copied\n", linked, copied);
	put(here + L"\\d3d9.dll", dest + L"\\d3d9.dll");
	copy_tree(here + L"\\cojvr", dest + L"\\cojvr", false, false, linked, copied);
	write_file(dest + L"\\steam_appid.txt", "204450");
	write_file(marker, "CoJ Gunslinger VR copy made by cojvr-setup.exe\r\n");
	wstring launcher = dest + L"\\cojvr-launch.exe";
	put(self_path(), launcher);
	wait_steam_closed();
	int n = set_shortcut(root, launcher, dest);
	printf("\nDone. \"%s\" was added to Steam (%d user%s).\nStart SteamVR first, then the game from Steam. The normal game is unchanged.\n\nPress Enter to close.",
	       APP_NAME, n, n == 1 ? "" : "s");
	getchar();
	return 0;
}
