#include "provider.h"

CProvider::CProvider()
    : _cRef(1), _cpus(CPUS_INVALID), _active(false), _failed(false), _port(PU_DEFAULT_PORT),
      _pCred(nullptr), _pEvents(nullptr), _adviseContext(0), _hThread(nullptr), _hStop(nullptr), _trigger(0)
{
    InitializeCriticalSection(&_cs);
    DllAddRef();
}

CProvider::~CProvider()
{
    StopListener();
    if (_pEvents) _pEvents->Release();
    if (_pCred) _pCred->Release();
    PU_Wipe(_key);
    DeleteCriticalSection(&_cs);
    DllRelease();
}

IFACEMETHODIMP CProvider::QueryInterface(REFIID riid, void** ppv)
{
    static const QITAB qit[] = {
        QITABENT(CProvider, ICredentialProvider),
        QITABENT(CProvider, ICredentialProviderSetUserArray),
        { 0 },
    };
    return QISearch(this, qit, riid, ppv);
}

IFACEMETHODIMP CProvider::SetUsageScenario(CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, DWORD)
{
    // Only the lock screen / sign-in screen. Never CredUI or change-password.
    if (cpus != CPUS_LOGON && cpus != CPUS_UNLOCK_WORKSTATION)
        return E_NOTIMPL;

    // Turned off by the user in the app -> no tile at all.
    if (PU_GetFlag(PU_FLAG_PAUSED))
        return E_NOTIMPL;

    try
    {
        // Not configured -> stay out of the way entirely.
        if (!PU_ReadConfig(_sid, _port))
            return E_NOTIMPL;
        if (!PU_ReadSecret(PU_SECRET_KEY, _key) || _key.size() < 16)
        {
            PU_Wipe(_key);
            return E_NOTIMPL;
        }
    }
    catch (...)
    {
        return E_NOTIMPL;
    }

    _failed = PU_GetFlag(PU_FLAG_FAILED);
    _cpus = cpus;
    _active = true;
    return S_OK;
}

IFACEMETHODIMP CProvider::SetUserArray(ICredentialProviderUserArray* users)
{
    if (_pCred)
    {
        _pCred->Release();
        _pCred = nullptr;
    }
    if (!_active || !users)
        return S_OK;

    DWORD count = 0;
    if (FAILED(users->GetCount(&count)))
        return S_OK;

    for (DWORD i = 0; i < count && !_pCred; i++)
    {
        ICredentialProviderUser* user = nullptr;
        if (FAILED(users->GetAt(i, &user)) || !user)
            continue;

        PWSTR sid = nullptr;
        if (SUCCEEDED(user->GetSid(&sid)) && sid)
        {
            if (_wcsicmp(sid, _sid.c_str()) == 0)
            {
                PWSTR qualified = nullptr;
                if (SUCCEEDED(user->GetStringValue(PKEY_Identity_QualifiedUserName, &qualified)) && qualified)
                {
                    CCredential::Create(_cpus, sid, qualified, _failed, &_pCred);
                    CoTaskMemFree(qualified);
                }
            }
            CoTaskMemFree(sid);
        }
        user->Release();
    }
    return S_OK;
}

IFACEMETHODIMP CProvider::Advise(ICredentialProviderEvents* pcpe, UINT_PTR upAdviseContext)
{
    EnterCriticalSection(&_cs);
    if (_pEvents) _pEvents->Release();
    _pEvents = pcpe;
    if (_pEvents) _pEvents->AddRef();
    _adviseContext = upAdviseContext;
    LeaveCriticalSection(&_cs);

    if (_active && !_failed)
        StartListener();
    return S_OK;
}

IFACEMETHODIMP CProvider::UnAdvise()
{
    StopListener();

    EnterCriticalSection(&_cs);
    if (_pEvents) _pEvents->Release();
    _pEvents = nullptr;
    _adviseContext = 0;
    LeaveCriticalSection(&_cs);
    return S_OK;
}

IFACEMETHODIMP CProvider::GetFieldDescriptorCount(DWORD* pdwCount)
{
    *pdwCount = FI_NUM_FIELDS;
    return S_OK;
}

IFACEMETHODIMP CProvider::GetFieldDescriptorAt(DWORD dwIndex, CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR** ppcpfd)
{
    *ppcpfd = nullptr;
    if (dwIndex >= FI_NUM_FIELDS)
        return E_INVALIDARG;

    auto* d = (CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR*)CoTaskMemAlloc(sizeof(CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR));
    if (!d) return E_OUTOFMEMORY;
    ZeroMemory(d, sizeof(*d));
    d->dwFieldID = dwIndex;

    PCWSTR label = L"Phone Unlock";
    switch (dwIndex)
    {
    case FI_TILEIMAGE:
        d->cpft = CPFT_TILE_IMAGE;
        d->guidFieldType = CPFG_CREDENTIAL_PROVIDER_LOGO;
        label = L"Image";
        break;
    case FI_LABEL:
        d->cpft = CPFT_SMALL_TEXT;
        d->guidFieldType = CPFG_CREDENTIAL_PROVIDER_LABEL;
        break;
    case FI_LARGETEXT:
        d->cpft = CPFT_LARGE_TEXT;
        break;
    }

    HRESULT hr = SHStrDupW(label, &d->pszLabel);
    if (FAILED(hr))
    {
        CoTaskMemFree(d);
        return hr;
    }
    *ppcpfd = d;
    return S_OK;
}

IFACEMETHODIMP CProvider::GetCredentialCount(DWORD* pdwCount, DWORD* pdwDefault, BOOL* pbAutoLogonWithDefault)
{
    *pdwCount = _pCred ? 1 : 0;
    *pdwDefault = CREDENTIAL_PROVIDER_NO_DEFAULT;
    *pbAutoLogonWithDefault = FALSE;

    if (InterlockedExchange(&_trigger, 0) && _pCred && !_failed)
    {
        *pdwDefault = 0;
        *pbAutoLogonWithDefault = TRUE;
    }
    return S_OK;
}

IFACEMETHODIMP CProvider::GetCredentialAt(DWORD dwIndex, ICredentialProviderCredential** ppcpc)
{
    *ppcpc = nullptr;
    if (dwIndex != 0 || !_pCred)
        return E_INVALIDARG;
    return _pCred->QueryInterface(IID_PPV_ARGS(ppcpc));
}

void CProvider::StartListener()
{
    if (_hThread) return;
    _hStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!_hStop) return;
    _hThread = CreateThread(nullptr, 0, ListenThread, this, 0, nullptr);
    if (!_hThread)
    {
        CloseHandle(_hStop);
        _hStop = nullptr;
    }
}

void CProvider::StopListener()
{
    if (_hThread)
    {
        SetEvent(_hStop);
        WaitForSingleObject(_hThread, 5000);
        CloseHandle(_hThread);
        _hThread = nullptr;
    }
    if (_hStop)
    {
        CloseHandle(_hStop);
        _hStop = nullptr;
    }
}

void CProvider::OnTrigger()
{
    InterlockedExchange(&_trigger, 1);

    EnterCriticalSection(&_cs);
    ICredentialProviderEvents* ev = _pEvents;
    UINT_PTR ctx = _adviseContext;
    if (ev) ev->AddRef();
    LeaveCriticalSection(&_cs);

    if (ev)
    {
        ev->CredentialsChanged(ctx);
        ev->Release();
    }
}

// ---- UDP (Wi-Fi / LAN) ------------------------------------------------------

static SOCKET OpenUdp(DWORD port)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;

    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0)
    {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

// ---- Bluetooth RFCOMM ---------------------------------------------------------

// Reads one packet from an accepted RFCOMM connection, replies 1 (accepted) or 0, closes.
static bool HandleBtClient(SOCKET c, const std::vector<BYTE>& key)
{
    WSAEventSelect(c, nullptr, 0);
    u_long blocking = 0;
    ioctlsocket(c, FIONBIO, &blocking);
    DWORD timeout = 3000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    BYTE buf[PU_PACKET_LEN];
    int got = 0;
    while (got < PU_PACKET_LEN)
    {
        int r = recv(c, (char*)buf + got, PU_PACKET_LEN - got, 0);
        if (r <= 0) break;
        got += r;
    }

    ULONGLONG ts = 0;
    bool ok = got == PU_PACKET_LEN && PU_VerifyPacket(buf, got, key, &ts) && PU_AcceptTimestamp(ts);
    char ack = ok ? 1 : 0;
    send(c, &ack, 1, 0);
    shutdown(c, SD_SEND);
    closesocket(c);
    return ok;
}

DWORD WINAPI CProvider::ListenThread(LPVOID param)
{
    CProvider* self = (CProvider*)param;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 1;

    SOCKET udp = OpenUdp(self->_port);
    PU_BtServer bt;
    PU_BtOpen(bt, PU_BT_UNLOCK_UUID, L"PhoneUnlock");

    WSAEVENT evUdp = WSACreateEvent();
    WSAEVENT evBt = WSACreateEvent();
    if (udp != INVALID_SOCKET && evUdp != WSA_INVALID_EVENT) WSAEventSelect(udp, evUdp, FD_READ);
    if (bt.s != INVALID_SOCKET && evBt != WSA_INVALID_EVENT) WSAEventSelect(bt.s, evBt, FD_ACCEPT);

    if ((udp != INVALID_SOCKET || bt.s != INVALID_SOCKET) &&
        evUdp != WSA_INVALID_EVENT && evBt != WSA_INVALID_EVENT)
    {
        HANDLE handles[3] = { self->_hStop, evUdp, evBt };
        for (;;)
        {
            DWORD r = WaitForMultipleObjects(3, handles, FALSE, INFINITE);
            if (r == WAIT_OBJECT_0 + 1)
            {
                WSAResetEvent(evUdp);
                for (;;)
                {
                    BYTE buf[128];
                    int n = recvfrom(udp, (char*)buf, sizeof(buf), 0, nullptr, nullptr);
                    if (n == SOCKET_ERROR) break;
                    ULONGLONG ts = 0;
                    if (PU_VerifyPacket(buf, n, self->_key, &ts) && PU_AcceptTimestamp(ts))
                        self->OnTrigger();
                }
            }
            else if (r == WAIT_OBJECT_0 + 2)
            {
                WSAResetEvent(evBt);
                for (;;)
                {
                    SOCKET c = accept(bt.s, nullptr, nullptr);
                    if (c == INVALID_SOCKET) break;
                    if (HandleBtClient(c, self->_key))
                        self->OnTrigger();
                }
            }
            else
            {
                break;
            }
        }
    }

    PU_BtClose(bt);
    if (udp != INVALID_SOCKET) closesocket(udp);
    if (evUdp != WSA_INVALID_EVENT) WSACloseEvent(evUdp);
    if (evBt != WSA_INVALID_EVENT) WSACloseEvent(evBt);
    WSACleanup();
    return 0;
}
