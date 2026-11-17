#include "credential.h"

CCredential::CCredential() : _cRef(1), _cpus(CPUS_INVALID), _failed(false)
{
    DllAddRef();
}

CCredential::~CCredential()
{
    DllRelease();
}

HRESULT CCredential::Create(CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, PCWSTR sid, PCWSTR qualifiedName,
                            bool failed, CCredential** out)
{
    *out = nullptr;
    CCredential* c = new (std::nothrow) CCredential();
    if (!c) return E_OUTOFMEMORY;
    try
    {
        c->_cpus = cpus;
        c->_sid = sid;
        c->_qualifiedName = qualifiedName;
        c->_failed = failed;
    }
    catch (...)
    {
        c->Release();
        return E_OUTOFMEMORY;
    }
    *out = c;
    return S_OK;
}

IFACEMETHODIMP CCredential::QueryInterface(REFIID riid, void** ppv)
{
    static const QITAB qit[] = {
        QITABENT(CCredential, ICredentialProviderCredential),
        QITABENT(CCredential, ICredentialProviderCredential2),
        { 0 },
    };
    return QISearch(this, qit, riid, ppv);
}

IFACEMETHODIMP CCredential::GetFieldState(DWORD dwFieldID, CREDENTIAL_PROVIDER_FIELD_STATE* pcpfs,
                                          CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE* pcpfis)
{
    if (!pcpfs || !pcpfis) return E_INVALIDARG;
    *pcpfis = CPFIS_NONE;
    switch (dwFieldID)
    {
    case FI_TILEIMAGE: *pcpfs = CPFS_DISPLAY_IN_BOTH; return S_OK;
    case FI_LABEL:     *pcpfs = CPFS_HIDDEN; return S_OK;
    case FI_LARGETEXT: *pcpfs = CPFS_DISPLAY_IN_SELECTED_TILE; return S_OK;
    }
    return E_INVALIDARG;
}

IFACEMETHODIMP CCredential::GetStringValue(DWORD dwFieldID, PWSTR* ppsz)
{
    if (!ppsz) return E_INVALIDARG;
    *ppsz = nullptr;
    switch (dwFieldID)
    {
    case FI_LABEL:
        return SHStrDupW(L"Phone Unlock", ppsz);
    case FI_LARGETEXT:
        return SHStrDupW(_failed
            ? L"Phone Unlock is paused: Windows rejected the saved password. Save it again in the Phone Unlock app."
            : L"Unlock your phone to sign in", ppsz);
    }
    return E_INVALIDARG;
}

// Draws a simple white phone outline on a blue square.
static HBITMAP MakeLogo()
{
    const int W = 128, H = 128;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm || !bits) return hbm;

    DWORD* px = (DWORD*)bits;
    const DWORD bg = 0xFF0063B1, fg = 0xFFFFFFFF;
    for (int y = 0; y < H; y++)
    {
        for (int x = 0; x < W; x++)
        {
            bool outer = x >= 40 && x < 88 && y >= 16 && y < 112;
            bool inner = x >= 46 && x < 82 && y >= 24 && y < 96;
            int dx = x - 64, dy = y - 104;
            bool button = dx * dx + dy * dy <= 16;
            px[y * W + x] = ((outer && !inner && !button) ? fg : bg);
        }
    }
    return hbm;
}

IFACEMETHODIMP CCredential::GetBitmapValue(DWORD dwFieldID, HBITMAP* phbmp)
{
    if (!phbmp) return E_INVALIDARG;
    *phbmp = nullptr;
    if (dwFieldID != FI_TILEIMAGE) return E_INVALIDARG;
    *phbmp = MakeLogo();
    return *phbmp ? S_OK : E_OUTOFMEMORY;
}

IFACEMETHODIMP CCredential::GetUserSid(PWSTR* ppszSid)
{
    if (!ppszSid) return E_INVALIDARG;
    return SHStrDupW(_sid.c_str(), ppszSid);
}

// CredProtect the password (LogonUI expects protected passwords in serialized credentials).
static HRESULT ProtectPassword(const std::wstring& in, std::wstring& out)
{
    out.clear();
    if (in.empty()) return S_OK;

    std::vector<wchar_t> src(in.begin(), in.end());
    src.push_back(L'\0');

    HRESULT hr = S_OK;
    CRED_PROTECTION_TYPE type;
    if (CredIsProtectedW(src.data(), &type) && type != CredUnprotected)
    {
        out = in;
    }
    else
    {
        DWORD cch = 0;
        if (!CredProtectW(FALSE, src.data(), (DWORD)src.size(), nullptr, &cch, nullptr) &&
            GetLastError() == ERROR_INSUFFICIENT_BUFFER && cch > 0)
        {
            std::vector<wchar_t> buf(cch);
            if (CredProtectW(FALSE, src.data(), (DWORD)src.size(), buf.data(), &cch, nullptr))
                out.assign(buf.data());
            else
                hr = HRESULT_FROM_WIN32(GetLastError());
            SecureZeroMemory(buf.data(), buf.size() * sizeof(wchar_t));
        }
        else
        {
            hr = E_FAIL;
        }
    }
    SecureZeroMemory(src.data(), src.size() * sizeof(wchar_t));
    return hr;
}

// Packs a KERB_INTERACTIVE_UNLOCK_LOGON with buffer pointers replaced by offsets.
static HRESULT PackLogon(KERB_LOGON_SUBMIT_TYPE type, const std::wstring& domain, const std::wstring& user,
                         const std::wstring& password, BYTE** ppb, DWORD* pcb)
{
    const size_t cbD = domain.size() * sizeof(wchar_t);
    const size_t cbU = user.size() * sizeof(wchar_t);
    const size_t cbP = password.size() * sizeof(wchar_t);
    if (cbD > 0xFFFE || cbU > 0xFFFE || cbP > 0xFFFE) return E_INVALIDARG;

    DWORD cb = (DWORD)(sizeof(KERB_INTERACTIVE_UNLOCK_LOGON) + cbD + cbU + cbP);
    BYTE* buf = (BYTE*)CoTaskMemAlloc(cb);
    if (!buf) return E_OUTOFMEMORY;
    ZeroMemory(buf, cb);

    KERB_INTERACTIVE_UNLOCK_LOGON* kiul = (KERB_INTERACTIVE_UNLOCK_LOGON*)buf;
    KERB_INTERACTIVE_LOGON* kil = &kiul->Logon;
    kil->MessageType = type;

    BYTE* p = buf + sizeof(KERB_INTERACTIVE_UNLOCK_LOGON);
    auto put = [&](UNICODE_STRING& us, const std::wstring& s, size_t cbs) {
        us.Length = us.MaximumLength = (USHORT)cbs;
        us.Buffer = (PWSTR)(ULONG_PTR)(p - buf);
        if (cbs) memcpy(p, s.data(), cbs);
        p += cbs;
    };
    put(kil->LogonDomainName, domain, cbD);
    put(kil->UserName, user, cbU);
    put(kil->Password, password, cbP);

    *ppb = buf;
    *pcb = cb;
    return S_OK;
}

static HRESULT LookupNegotiatePackage(ULONG* pkg)
{
    HANDLE hLsa;
    if (LsaConnectUntrusted(&hLsa) != STATUS_SUCCESS) return E_FAIL;
    LSA_STRING name;
    name.Buffer = (PCHAR)"Negotiate";
    name.Length = (USHORT)strlen(name.Buffer);
    name.MaximumLength = name.Length + 1;
    NTSTATUS st = LsaLookupAuthenticationPackage(hLsa, &name, pkg);
    LsaDeregisterLogonProcess(hLsa);
    return st == STATUS_SUCCESS ? S_OK : HRESULT_FROM_NT(st);
}

IFACEMETHODIMP CCredential::GetSerialization(CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE* pcpgsr,
                                             CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs,
                                             PWSTR* ppszOptionalStatusText,
                                             CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon)
{
    *pcpgsr = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
    *ppszOptionalStatusText = nullptr;
    *pcpsiOptionalStatusIcon = CPSI_NONE;
    ZeroMemory(pcpcs, sizeof(*pcpcs));

    if (_failed)
    {
        SHStrDupW(L"Phone Unlock is paused. Use your PIN or password.", ppszOptionalStatusText);
        *pcpsiOptionalStatusIcon = CPSI_WARNING;
        return S_OK;
    }

    HRESULT hr = E_FAIL;
    try
    {
        std::vector<BYTE> raw;
        if (!PU_ReadSecret(PU_SECRET_PWD, raw) || raw.size() % 2 != 0)
        {
            PU_Wipe(raw);
            SHStrDupW(L"Phone Unlock is not set up. Use your PIN or password.", ppszOptionalStatusText);
            *pcpsiOptionalStatusIcon = CPSI_ERROR;
            return S_OK;
        }
        std::wstring password((const wchar_t*)raw.data(), raw.size() / sizeof(wchar_t));
        PU_Wipe(raw);

        std::wstring domain, user;
        size_t pos = _qualifiedName.find(L'\\');
        if (pos != std::wstring::npos)
        {
            domain = _qualifiedName.substr(0, pos);
            user = _qualifiedName.substr(pos + 1);
        }
        else
        {
            WCHAR comp[MAX_COMPUTERNAME_LENGTH + 1];
            DWORD n = ARRAYSIZE(comp);
            if (GetComputerNameW(comp, &n)) domain = comp;
            user = _qualifiedName;
        }

        std::wstring protectedPwd;
        hr = ProtectPassword(password, protectedPwd);
        PU_Wipe(password);

        if (SUCCEEDED(hr))
        {
            KERB_LOGON_SUBMIT_TYPE type =
                (_cpus == CPUS_UNLOCK_WORKSTATION) ? KerbWorkstationUnlockLogon : KerbInteractiveLogon;
            hr = PackLogon(type, domain, user, protectedPwd, &pcpcs->rgbSerialization, &pcpcs->cbSerialization);
        }
        PU_Wipe(protectedPwd);

        if (SUCCEEDED(hr))
        {
            ULONG pkg = 0;
            hr = LookupNegotiatePackage(&pkg);
            if (SUCCEEDED(hr))
            {
                pcpcs->ulAuthenticationPackage = pkg;
                pcpcs->clsidCredentialProvider = CLSID_PhoneUnlockProvider;
                *pcpgsr = CPGSR_RETURN_CREDENTIAL_FINISHED;
            }
            else
            {
                SecureZeroMemory(pcpcs->rgbSerialization, pcpcs->cbSerialization);
                CoTaskMemFree(pcpcs->rgbSerialization);
                pcpcs->rgbSerialization = nullptr;
                pcpcs->cbSerialization = 0;
            }
        }
    }
    catch (...)
    {
        hr = E_OUTOFMEMORY;
    }

    if (FAILED(hr))
    {
        SHStrDupW(L"Phone Unlock failed. Use your PIN or password.", ppszOptionalStatusText);
        *pcpsiOptionalStatusIcon = CPSI_ERROR;
    }
    return S_OK;
}

IFACEMETHODIMP CCredential::ReportResult(NTSTATUS ntsStatus, NTSTATUS,
                                         PWSTR* ppszOptionalStatusText,
                                         CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon)
{
    *ppszOptionalStatusText = nullptr;
    *pcpsiOptionalStatusIcon = CPSI_NONE;
    if (ntsStatus < 0)
    {
        // Stop auto sign-in so repeated phone unlocks can't lock the account out.
        PU_SetFlag(PU_FLAG_FAILED, true);
        _failed = true;
        SHStrDupW(L"Phone Unlock: Windows rejected the saved password. It is paused until you save the password again in the Phone Unlock app.",
                  ppszOptionalStatusText);
        *pcpsiOptionalStatusIcon = CPSI_ERROR;
    }
    return S_OK;
}
