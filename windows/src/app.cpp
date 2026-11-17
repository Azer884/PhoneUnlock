// PhoneUnlock.exe - setup and control app for the Phone Unlock credential provider.
//
//   PhoneUnlock.exe              open the app (installs itself on first use)
//   PhoneUnlock.exe /uninstall   remove everything (used by Settings > Apps)
//   PhoneUnlock.exe /selftest    load the provider and send it a signed packet over loopback (exit code 0 = pass)
#include "common.h"
#include "resource.h"
#include <commctrl.h>
#include <sddl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <propvarutil.h>
#include <stdio.h>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

void DllAddRef() {}
void DllRelease() {}

#define PU_GITHUB_URL   L"https://github.com/Azer884/PhoneUnlock"
#define PU_CP_KEY       L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\Credential Providers\\" PU_CLSID_STR
#define PU_CLSID_KEY    L"SOFTWARE\\Classes\\CLSID\\" PU_CLSID_STR
#define PU_UNINST_KEY   L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PhoneUnlock"
#define PU_HELLO_KEY    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\PasswordLess\\Device"
#define PU_FW_RULE      L"PhoneUnlock"

#define WM_APP_TEXT     (WM_APP + 1)   // wParam = control id, lParam = new wchar_t[] (freed by UI)
#define WM_APP_DONE     (WM_APP + 2)   // a worker finished

static HINSTANCE g_hinst;
static HWND g_dlg;
static HANDLE g_worker;
static HANDLE g_cancel;
static int g_workerButton;             // IDC_PAIR or IDC_TEST while running

// ============================================================================ helpers

static std::wstring InstallDir()
{
    PWSTR pf = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &pf)))
        dir = std::wstring(pf) + L"\\PhoneUnlock";
    CoTaskMemFree(pf);
    return dir;
}

static std::wstring SelfPath()
{
    WCHAR p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

static std::wstring ShortcutPath()
{
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_CommonPrograms, 0, nullptr, &p)))
        s = std::wstring(p) + L"\\Phone Unlock.lnk";
    CoTaskMemFree(p);
    return s;
}

static void PostText(int id, const std::wstring& text)
{
    wchar_t* copy = new (std::nothrow) wchar_t[text.size() + 1];
    if (!copy) return;
    wcscpy_s(copy, text.size() + 1, text.c_str());
    if (!PostMessageW(g_dlg, WM_APP_TEXT, id, (LPARAM)copy))
        delete[] copy;
}

static bool RegSetString(const wchar_t* key, const wchar_t* name, const std::wstring& value)
{
    return RegSetKeyValueW(HKEY_LOCAL_MACHINE, key, name, REG_SZ, value.c_str(),
                           (DWORD)((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

static bool RegSetDword(const wchar_t* key, const wchar_t* name, DWORD value)
{
    return RegSetKeyValueW(HKEY_LOCAL_MACHINE, key, name, REG_DWORD, &value, sizeof(value)) == ERROR_SUCCESS;
}

static bool RegKeyExists(const wchar_t* key)
{
    HKEY hk;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &hk) != ERROR_SUCCESS) return false;
    RegCloseKey(hk);
    return true;
}

static DWORD RunHidden(const std::wstring& cmdline)
{
    std::wstring cmd = cmdline;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return GetLastError();
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

static bool StoreSecret(const wchar_t* name, const void* data, size_t cb)
{
    LSA_OBJECT_ATTRIBUTES oa = {};
    LSA_HANDLE h = nullptr;
    if (LsaOpenPolicy(nullptr, &oa, POLICY_CREATE_SECRET, &h) != STATUS_SUCCESS)
        return false;

    LSA_UNICODE_STRING key;
    key.Buffer = const_cast<PWSTR>(name);
    key.Length = (USHORT)(wcslen(name) * sizeof(WCHAR));
    key.MaximumLength = key.Length + sizeof(WCHAR);

    LSA_UNICODE_STRING val;
    val.Buffer = (PWSTR)data;
    val.Length = val.MaximumLength = (USHORT)cb;

    NTSTATUS st = LsaStorePrivateData(h, &key, data ? &val : nullptr);
    LsaClose(h);
    return st == STATUS_SUCCESS || (!data && st == STATUS_OBJECT_NAME_NOT_FOUND);
}

static bool HasSecret(const wchar_t* name)
{
    std::vector<BYTE> v;
    bool ok = PU_ReadSecret(name, v);
    PU_Wipe(v);
    return ok;
}

static bool GetCurrentUserSid(std::wstring& sid)
{
    HANDLE hTok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hTok)) return false;
    BYTE buf[256];
    DWORD cb = 0;
    bool ok = false;
    if (GetTokenInformation(hTok, TokenUser, buf, sizeof(buf), &cb))
    {
        PWSTR s = nullptr;
        if (ConvertSidToStringSidW(((TOKEN_USER*)buf)->User.Sid, &s))
        {
            sid = s;
            LocalFree(s);
            ok = true;
        }
    }
    CloseHandle(hTok);
    return ok;
}

static std::wstring FormatCode(DWORD code)
{
    WCHAR b[16];
    StringCchPrintfW(b, ARRAYSIZE(b), L"%03lu %03lu", code / 1000, code % 1000);
    return b;
}

// ============================================================================ state

struct State
{
    bool installed = false;
    bool password = false;
    bool paired = false;
    bool failed = false;
    bool paused = false;
    bool helloOnly = false;
};

static State ReadState()
{
    State s;
    s.installed = RegKeyExists(PU_CP_KEY) &&
                  GetFileAttributesW((InstallDir() + L"\\PhoneUnlockCP.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
    s.password = HasSecret(PU_SECRET_PWD);
    s.paired = HasSecret(PU_SECRET_KEY);
    s.failed = PU_GetFlag(PU_FLAG_FAILED);
    s.paused = PU_GetFlag(PU_FLAG_PAUSED);

    DWORD v = 0, cb = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, PU_HELLO_KEY, L"DevicePasswordLessBuildVersion", RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS)
        s.helloOnly = v == 2;
    return s;
}

static void RefreshUi()
{
    State s = ReadState();
    std::wstring t;
    if (!s.installed)
    {
        t = L"Not installed yet. Click Install.";
    }
    else
    {
        t += s.password ? (s.failed ? L"Password: REJECTED by Windows. Save it again.\n" : L"Password: saved\n")
                        : L"Password: not saved yet\n";
        t += s.paired ? L"Phone: paired\n" : L"Phone: not paired yet\n";
        if (s.helloOnly)
            t += L"Warning: \"Only allow Windows Hello sign-in\" is on. Click Install again to fix.";
        else if (s.paused)
            t += L"Status: turned off";
        else if (s.password && s.paired && !s.failed)
            t += L"Status: ready. Lock the PC, then unlock your phone.";
        else
            t += L"Status: finish the steps below.";
    }
    SetDlgItemTextW(g_dlg, IDC_STATUS, t.c_str());
    SetDlgItemTextW(g_dlg, IDC_INSTALL, s.installed ? L"Repair" : L"Install");

    bool busy = g_worker != nullptr;
    int ids[] = { IDC_PASSWORD, IDC_SAVEPWD, IDC_PAIR, IDC_TEST, IDC_ENABLED, IDC_UNINSTALL };
    for (int id : ids)
        EnableWindow(GetDlgItem(g_dlg, id), s.installed && (!busy || id == g_workerButton));
    EnableWindow(GetDlgItem(g_dlg, IDC_INSTALL), !busy);
    CheckDlgButton(g_dlg, IDC_ENABLED, s.paused ? BST_UNCHECKED : BST_CHECKED);
}

// ============================================================================ install / uninstall

static bool WriteProviderDll(const std::wstring& path, std::wstring& err)
{
    HRSRC res = FindResourceW(g_hinst, MAKEINTRESOURCEW(IDR_PROVIDER_DLL), RT_RCDATA);
    HGLOBAL mem = res ? LoadResource(g_hinst, res) : nullptr;
    const void* data = mem ? LockResource(mem) : nullptr;
    DWORD size = res ? SizeofResource(g_hinst, res) : 0;
    if (!data || !size)
    {
        err = L"The provider DLL is missing from this program.";
        return false;
    }

    std::wstring tmp = path + L".new";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        err = L"Could not write " + tmp;
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(f, data, size, &written, nullptr) && written == size;
    CloseHandle(f);
    if (!ok)
    {
        DeleteFileW(tmp.c_str());
        err = L"Could not write " + tmp;
        return false;
    }

    // The lock screen may have the old DLL loaded: it can't be overwritten, but it can be renamed.
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && !DeleteFileW(path.c_str()))
    {
        WCHAR old[MAX_PATH];
        StringCchPrintfW(old, MAX_PATH, L"%s.old-%llu", path.c_str(), GetTickCount64());
        if (MoveFileExW(path.c_str(), old, MOVEFILE_REPLACE_EXISTING))
            MoveFileExW(old, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        err = L"Could not replace " + path;
        return false;
    }
    return true;
}

static void CreateShortcut(const std::wstring& target)
{
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
        return;
    link->SetPath(target.c_str());
    link->SetDescription(L"Unlock this PC with your Android phone");
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&pf))))
    {
        pf->Save(ShortcutPath().c_str(), TRUE);
        pf->Release();
    }
    link->Release();
}

static bool Install(std::wstring& err)
{
    std::wstring dir = InstallDir();
    if (dir.empty() || (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS))
    {
        err = L"Could not create " + dir;
        return false;
    }

    // 1. Program files
    std::wstring exe = dir + L"\\PhoneUnlock.exe";
    std::wstring self = SelfPath();
    if (_wcsicmp(self.c_str(), exe.c_str()) != 0 && !CopyFileW(self.c_str(), exe.c_str(), FALSE))
    {
        err = L"Could not copy the app to " + exe + L". Close other Phone Unlock windows and try again.";
        return false;
    }
    std::wstring dll = dir + L"\\PhoneUnlockCP.dll";
    if (!WriteProviderDll(dll, err))
        return false;

    // 2. Register the credential provider
    bool ok = RegSetString(PU_CLSID_KEY, nullptr, L"PhoneUnlock") &&
              RegSetString(PU_CLSID_KEY L"\\InprocServer32", nullptr, dll) &&
              RegSetString(PU_CLSID_KEY L"\\InprocServer32", L"ThreadingModel", L"Apartment") &&
              RegSetString(PU_CP_KEY, nullptr, L"PhoneUnlock");
    if (!ok)
    {
        err = L"Could not register the credential provider.";
        return false;
    }

    // 3. Settings for this user
    std::wstring sid;
    if (!GetCurrentUserSid(sid))
    {
        err = L"Could not read your user account.";
        return false;
    }
    RegSetString(PU_REG_KEY, L"UserSid", sid);
    RegSetDword(PU_REG_KEY, L"Port", PU_DEFAULT_PORT);

    // 4. Firewall: Wi-Fi signals from the local network only
    RunHidden(L"netsh advfirewall firewall delete rule name=\"" PU_FW_RULE L"\"");
    RunHidden(L"netsh advfirewall firewall add rule name=\"" PU_FW_RULE L"\" dir=in action=allow protocol=UDP localport=47600 remoteip=localsubnet");

    // 5. Start menu + Settings > Apps entry
    CreateShortcut(exe);
    RegSetString(PU_UNINST_KEY, L"DisplayName", L"Phone Unlock");
    RegSetString(PU_UNINST_KEY, L"DisplayVersion", PU_VERSION_STR);
    RegSetString(PU_UNINST_KEY, L"Publisher", L"Phone Unlock");
    RegSetString(PU_UNINST_KEY, L"DisplayIcon", exe);
    RegSetString(PU_UNINST_KEY, L"InstallLocation", dir);
    RegSetString(PU_UNINST_KEY, L"URLInfoAbout", PU_GITHUB_URL);
    RegSetString(PU_UNINST_KEY, L"UninstallString", L"\"" + exe + L"\" /uninstall");
    RegSetDword(PU_UNINST_KEY, L"NoModify", 1);
    RegSetDword(PU_UNINST_KEY, L"NoRepair", 1);

    // 6. "Only allow Windows Hello sign-in" blocks every non-Hello sign-in method, including this one.
    State s = ReadState();
    if (s.helloOnly &&
        MessageBoxW(g_dlg,
                    L"\"Only allow Windows Hello sign-in\" is turned on. It blocks Phone Unlock.\n\n"
                    L"Turn it off? Your PIN and fingerprint keep working; a password option is added to the lock screen.",
                    L"Phone Unlock", MB_YESNO | MB_ICONQUESTION) == IDYES)
    {
        RegSetDword(PU_HELLO_KEY, L"DevicePasswordLessBuildVersion", 0);
    }
    return true;
}

static void Uninstall()
{
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, PU_CP_KEY);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, PU_CP_KEY);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, PU_CLSID_KEY);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, PU_CLSID_KEY);
    RunHidden(L"netsh advfirewall firewall delete rule name=\"" PU_FW_RULE L"\"");
    StoreSecret(PU_SECRET_PWD, nullptr, 0);
    StoreSecret(PU_SECRET_KEY, nullptr, 0);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, PU_REG_KEY);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, PU_REG_KEY);
    DeleteFileW(ShortcutPath().c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, PU_UNINST_KEY);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, PU_UNINST_KEY);

    // Files that are in use (the running app, a DLL loaded by the lock screen) go away at the next restart.
    std::wstring dir = InstallDir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            if (!DeleteFileW(p.c_str()))
                MoveFileExW(p.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!RemoveDirectoryW(dir.c_str()))
        MoveFileExW(dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

// ============================================================================ password

static void SavePassword()
{
    WCHAR buf[512] = {};
    GetDlgItemTextW(g_dlg, IDC_PASSWORD, buf, ARRAYSIZE(buf));
    std::wstring pwd = buf;
    SecureZeroMemory(buf, sizeof(buf));
    SetDlgItemTextW(g_dlg, IDC_PASSWORD, L"");

    if (pwd.empty())
    {
        MessageBoxW(g_dlg, L"Type your Windows password first.", L"Phone Unlock", MB_ICONINFORMATION);
        return;
    }

    WCHAR user[256];
    DWORD cch = ARRAYSIZE(user);
    GetUserNameW(user, &cch);
    HANDLE hTok = nullptr;
    if (LogonUserW(user, L".", pwd.c_str(), LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &hTok))
    {
        CloseHandle(hTok);
    }
    else
    {
        WCHAR msg[512];
        StringCchPrintfW(msg, ARRAYSIZE(msg),
                         L"Windows did not accept this password (error %lu).\n\n"
                         L"Use your Microsoft account password (not your PIN). Save it anyway?",
                         GetLastError());
        if (MessageBoxW(g_dlg, msg, L"Phone Unlock", MB_YESNO | MB_ICONWARNING) != IDYES)
        {
            PU_Wipe(pwd);
            return;
        }
    }

    bool ok = StoreSecret(PU_SECRET_PWD, pwd.data(), pwd.size() * sizeof(wchar_t));
    PU_Wipe(pwd);
    if (ok)
        PU_SetFlag(PU_FLAG_FAILED, false);
    else
        MessageBoxW(g_dlg, L"Could not save the password.", L"Phone Unlock", MB_ICONERROR);
    RefreshUi();
}

// ============================================================================ pairing

// Waits for FD_* events on a socket or for cancel. Returns true if the socket is ready.
static bool WaitSocket(SOCKET s, long events, DWORD timeoutMs)
{
    WSAEVENT ev = WSACreateEvent();
    if (ev == WSA_INVALID_EVENT) return false;
    WSAEventSelect(s, ev, events);
    HANDLE hs[2] = { g_cancel, ev };
    DWORD r = WaitForMultipleObjects(2, hs, FALSE, timeoutMs);
    WSAEventSelect(s, nullptr, 0);
    WSACloseEvent(ev);
    return r == WAIT_OBJECT_0 + 1;
}

static DWORD WINAPI PairThread(LPVOID)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    std::vector<BYTE> key(PU_KEY_LEN);
    BCryptGenRandom(nullptr, key.data(), PU_KEY_LEN, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::wstring code = FormatCode(PU_PairCode(key));

    PU_BtServer bt;
    if (!PU_BtOpen(bt, PU_BT_PAIR_UUID, L"PhoneUnlock Pairing"))
    {
        PostText(IDC_PAIRSTATUS, L"Bluetooth is off or missing on this PC.");
    }
    else
    {
        PostText(IDC_PAIRSTATUS, L"Waiting for your phone... Tap \"Pair with PC\" in the app.\nCode: " + code);

        SOCKET c = INVALID_SOCKET;
        if (WaitSocket(bt.s, FD_ACCEPT, 180000))
            c = accept(bt.s, nullptr, nullptr);

        if (c == INVALID_SOCKET)
        {
            PostText(IDC_PAIRSTATUS, WaitForSingleObject(g_cancel, 0) == WAIT_OBJECT_0 ? L"Cancelled." : L"No phone connected. Try again.");
        }
        else
        {
            u_long blocking = 0;
            ioctlsocket(c, FIONBIO, &blocking);

            BYTE msg[PU_PAIR_LEN];
            memcpy(msg, PU_PAIR_MAGIC, 4);
            memcpy(msg + 4, key.data(), PU_KEY_LEN);
            int sent = send(c, (const char*)msg, PU_PAIR_LEN, 0);
            SecureZeroMemory(msg, sizeof(msg));

            PostText(IDC_PAIRSTATUS, L"Check that your phone shows code " + code + L" and tap Confirm there.");

            char answer = 0;
            bool confirmed = sent == PU_PAIR_LEN && WaitSocket(c, FD_READ | FD_CLOSE, 120000) &&
                             recv(c, &answer, 1, 0) == 1 && answer == 1;
            closesocket(c);

            if (confirmed && StoreSecret(PU_SECRET_KEY, key.data(), key.size()))
            {
                RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, PU_REG_KEY, L"LastTs");
                PostText(IDC_PAIRSTATUS, L"Paired. Code " + code + L".");
            }
            else
            {
                PostText(IDC_PAIRSTATUS, L"Pairing was not confirmed on the phone. Nothing changed.");
            }
        }
        PU_BtClose(bt);
    }

    PU_Wipe(key);
    WSACleanup();
    PostMessageW(g_dlg, WM_APP_DONE, 0, 0);
    return 0;
}

// ============================================================================ test (acts like the lock screen)

class MockEvents : public ICredentialProviderEvents
{
public:
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~MockEvents() { CloseHandle(ev); }
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        static const QITAB qit[] = { QITABENT(MockEvents, ICredentialProviderEvents), { 0 } };
        return QISearch(this, qit, riid, ppv);
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }
    IFACEMETHODIMP_(ULONG) Release() override { return 1; }
    IFACEMETHODIMP CredentialsChanged(UINT_PTR) override { SetEvent(ev); return S_OK; }
};

class MockUser : public ICredentialProviderUser
{
public:
    std::wstring sid, qualified;
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        static const QITAB qit[] = { QITABENT(MockUser, ICredentialProviderUser), { 0 } };
        return QISearch(this, qit, riid, ppv);
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }
    IFACEMETHODIMP_(ULONG) Release() override { return 1; }
    IFACEMETHODIMP GetSid(PWSTR* p) override { return SHStrDupW(sid.c_str(), p); }
    IFACEMETHODIMP GetProviderID(GUID* g) override { *g = GUID_NULL; return S_OK; }
    IFACEMETHODIMP GetStringValue(REFPROPERTYKEY key, PWSTR* p) override
    {
        if (IsEqualPropertyKey(key, PKEY_Identity_QualifiedUserName)) return SHStrDupW(qualified.c_str(), p);
        *p = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetValue(REFPROPERTYKEY, PROPVARIANT* pv) override { PropVariantInit(pv); return E_NOTIMPL; }
};

class MockUserArray : public ICredentialProviderUserArray
{
public:
    MockUser* user = nullptr;
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        static const QITAB qit[] = { QITABENT(MockUserArray, ICredentialProviderUserArray), { 0 } };
        return QISearch(this, qit, riid, ppv);
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }
    IFACEMETHODIMP_(ULONG) Release() override { return 1; }
    IFACEMETHODIMP SetProviderFilter(REFGUID) override { return S_OK; }
    IFACEMETHODIMP GetAccountOptions(CREDENTIAL_PROVIDER_ACCOUNT_OPTIONS* o) override { *o = CPAO_NONE; return S_OK; }
    IFACEMETHODIMP GetCount(DWORD* n) override { *n = 1; return S_OK; }
    IFACEMETHODIMP GetAt(DWORD i, ICredentialProviderUser** pp) override
    {
        if (i != 0) return E_INVALIDARG;
        *pp = user;
        return S_OK;
    }
};

typedef HRESULT(STDAPICALLTYPE* PFN_GETCLASSOBJECT)(REFCLSID, REFIID, void**);

static bool SendLoopbackPacket(DWORD port)
{
    std::vector<BYTE> key;
    if (!PU_ReadSecret(PU_SECRET_KEY, key)) return false;
    BYTE pkt[PU_PACKET_LEN];
    bool ok = PU_BuildPacket(key, pkt);
    PU_Wipe(key);
    if (!ok) return false;

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    int n = sendto(s, (const char*)pkt, sizeof(pkt), 0, (sockaddr*)&a, sizeof(a));
    closesocket(s);
    return n == (int)sizeof(pkt);
}

// Loads the installed provider exactly like LogonUI would and waits for a signal.
// Returns 0 on pass; writes progress through report().
template <typename Report>
static int RunProviderTest(bool loopback, Report report)
{
    std::wstring sid;
    DWORD port = 0;
    if (!PU_ReadConfig(sid, port))
    {
        report(L"Not installed.");
        return 1;
    }

    HMODULE dll = LoadLibraryW((InstallDir() + L"\\PhoneUnlockCP.dll").c_str());
    auto getClassObject = dll ? (PFN_GETCLASSOBJECT)GetProcAddress(dll, "DllGetClassObject") : nullptr;
    IClassFactory* cf = nullptr;
    ICredentialProvider* prov = nullptr;
    if (!getClassObject || FAILED(getClassObject(CLSID_PhoneUnlockProvider, IID_PPV_ARGS(&cf))) ||
        FAILED(cf->CreateInstance(nullptr, IID_PPV_ARGS(&prov))))
    {
        if (cf) cf->Release();
        if (dll) FreeLibrary(dll);
        report(L"Could not load the lock-screen plugin. Click Repair.");
        return 1;
    }
    cf->Release();

    int result = 1;
    if (FAILED(prov->SetUsageScenario(CPUS_UNLOCK_WORKSTATION, 0)))
    {
        report(PU_GetFlag(PU_FLAG_PAUSED) ? L"Phone Unlock is turned off." : L"Not paired yet.");
    }
    else
    {
        WCHAR sam[512];
        ULONG cchSam = ARRAYSIZE(sam);
        MockUser user;
        user.sid = sid;
        user.qualified = GetUserNameExW(NameSamCompatible, sam, &cchSam) ? sam : L".\\user";
        MockUserArray users;
        users.user = &user;

        ICredentialProviderSetUserArray* sua = nullptr;
        if (SUCCEEDED(prov->QueryInterface(IID_PPV_ARGS(&sua))))
        {
            sua->SetUserArray(&users);
            sua->Release();
        }

        MockEvents events;
        prov->Advise(&events, 1);
        Sleep(300);

        if (loopback)
            SendLoopbackPacket(port);
        else
            report(L"Now lock and unlock your phone (waiting 90 s)...");

        HANDLE hs[2] = { events.ev, g_cancel };
        DWORD r = WaitForMultipleObjects(g_cancel ? 2 : 1, hs, FALSE, loopback ? 5000 : 90000);
        if (r != WAIT_OBJECT_0)
        {
            report(r == WAIT_OBJECT_0 + 1 ? L"Cancelled." : L"No signal from the phone. Is it paired, with Bluetooth on?");
        }
        else
        {
            DWORD count = 0, def = 0;
            BOOL autoLogon = FALSE;
            prov->GetCredentialCount(&count, &def, &autoLogon);
            ICredentialProviderCredential* cred = nullptr;
            if (count == 1 && autoLogon && SUCCEEDED(prov->GetCredentialAt(0, &cred)))
            {
                CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE resp;
                CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION cs = {};
                PWSTR status = nullptr;
                CREDENTIAL_PROVIDER_STATUS_ICON icon;
                cred->GetSerialization(&resp, &cs, &status, &icon);
                bool ok = resp == CPGSR_RETURN_CREDENTIAL_FINISHED && cs.cbSerialization > 0;
                if (cs.rgbSerialization)
                {
                    SecureZeroMemory(cs.rgbSerialization, cs.cbSerialization);
                    CoTaskMemFree(cs.rgbSerialization);
                }
                if (ok)
                {
                    report(L"It works. The lock screen would sign you in now.");
                    result = 0;
                }
                else
                {
                    report(status ? status : L"Signal OK, but the saved password is missing.");
                }
                if (status) CoTaskMemFree(status);
                cred->Release();
            }
            else
            {
                report(L"Signal OK, but your account was not found. Click Repair.");
            }
        }
        prov->UnAdvise();
    }
    prov->Release();
    FreeLibrary(dll);
    return result;
}

static DWORD WINAPI TestThread(LPVOID)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    RunProviderTest(false, [](const wchar_t* msg) { PostText(IDC_TESTSTATUS, msg); });
    CoUninitialize();
    WSACleanup();
    PostMessageW(g_dlg, WM_APP_DONE, 0, 0);
    return 0;
}

// ============================================================================ dialog

static void StartWorker(int button, LPTHREAD_START_ROUTINE fn)
{
    ResetEvent(g_cancel);
    g_workerButton = button;
    g_worker = CreateThread(nullptr, 0, fn, nullptr, 0, nullptr);
    if (g_worker)
        SetDlgItemTextW(g_dlg, button, L"Cancel");
    RefreshUi();
}

static INT_PTR CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        g_dlg = hwnd;
        HICON big = (HICON)LoadImageW(g_hinst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 32, 32, 0);
        HICON smallIcon = (HICON)LoadImageW(g_hinst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 16, 16, 0);
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)smallIcon);
        SetDlgItemTextW(hwnd, IDC_VERSION, L"v" PU_VERSION_STR);
        RefreshUi();
        return TRUE;
    }

    case WM_APP_TEXT:
    {
        wchar_t* text = (wchar_t*)lp;
        SetDlgItemTextW(hwnd, (int)wp, text);
        delete[] text;
        return TRUE;
    }

    case WM_APP_DONE:
        if (g_worker)
        {
            WaitForSingleObject(g_worker, INFINITE);
            CloseHandle(g_worker);
            g_worker = nullptr;
        }
        SetDlgItemTextW(hwnd, IDC_PAIR, L"Pair");
        SetDlgItemTextW(hwnd, IDC_TEST, L"Test");
        g_workerButton = 0;
        RefreshUi();
        return TRUE;

    case WM_NOTIFY:
    {
        NMHDR* nm = (NMHDR*)lp;
        if (nm->idFrom == IDC_GITHUB && (nm->code == NM_CLICK || nm->code == NM_RETURN))
            ShellExecuteW(hwnd, L"open", PU_GITHUB_URL, nullptr, nullptr, SW_SHOWNORMAL);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_INSTALL:
        {
            std::wstring err;
            HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
            bool ok = Install(err);
            SetCursor(old);
            if (!ok) MessageBoxW(hwnd, err.c_str(), L"Phone Unlock", MB_ICONERROR);
            RefreshUi();
            return TRUE;
        }
        case IDC_SAVEPWD:
            SavePassword();
            return TRUE;
        case IDC_PAIR:
            if (g_worker) SetEvent(g_cancel);
            else StartWorker(IDC_PAIR, PairThread);
            return TRUE;
        case IDC_TEST:
            if (g_worker) SetEvent(g_cancel);
            else StartWorker(IDC_TEST, TestThread);
            return TRUE;
        case IDC_ENABLED:
            PU_SetFlag(PU_FLAG_PAUSED, IsDlgButtonChecked(hwnd, IDC_ENABLED) != BST_CHECKED);
            RefreshUi();
            return TRUE;
        case IDC_UNINSTALL:
            if (MessageBoxW(hwnd, L"Remove Phone Unlock from this PC? Your saved password and pairing are deleted.",
                            L"Phone Unlock", MB_YESNO | MB_ICONQUESTION) == IDYES)
            {
                Uninstall();
                MessageBoxW(hwnd, L"Phone Unlock was removed. Uninstall the app on your phone too.", L"Phone Unlock", MB_ICONINFORMATION);
                EndDialog(hwnd, 0);
            }
            return TRUE;
        case IDCANCEL:
            if (g_worker)
            {
                SetEvent(g_cancel);
                WaitForSingleObject(g_worker, 10000);
            }
            EndDialog(hwnd, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR cmdline, int)
{
    g_hinst = hinst;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    if (cmdline && _wcsicmp(cmdline, L"/uninstall") == 0)
    {
        if (MessageBoxW(nullptr, L"Remove Phone Unlock from this PC?", L"Phone Unlock", MB_YESNO | MB_ICONQUESTION) == IDYES)
        {
            Uninstall();
            MessageBoxW(nullptr, L"Phone Unlock was removed.", L"Phone Unlock", MB_ICONINFORMATION);
        }
        return 0;
    }
    if (cmdline && _wcsicmp(cmdline, L"/selftest") == 0)
        return RunProviderTest(true, [](const wchar_t*) {});

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_LINK_CLASS };
    InitCommonControlsEx(&icc);
    g_cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DialogBoxParamW(hinst, MAKEINTRESOURCEW(IDD_MAIN), nullptr, DlgProc, 0);
    return 0;
}
