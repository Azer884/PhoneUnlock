#include "common.h"

ULONGLONG PU_NowUnixMs()
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (u.QuadPart - 116444736000000000ULL) / 10000ULL;
}

void PU_Wipe(std::vector<BYTE>& v)
{
    if (!v.empty()) SecureZeroMemory(v.data(), v.size());
    v.clear();
}

void PU_Wipe(std::wstring& s)
{
    if (!s.empty()) SecureZeroMemory(&s[0], s.size() * sizeof(wchar_t));
    s.clear();
}

// Reads an LSA private secret. Requires SYSTEM or an elevated administrator.
bool PU_ReadSecret(const wchar_t* name, std::vector<BYTE>& out)
{
    out.clear();
    LSA_OBJECT_ATTRIBUTES oa = {};
    LSA_HANDLE hPolicy = nullptr;
    if (LsaOpenPolicy(nullptr, &oa, POLICY_GET_PRIVATE_INFORMATION, &hPolicy) != STATUS_SUCCESS)
        return false;

    LSA_UNICODE_STRING keyName;
    keyName.Buffer = const_cast<PWSTR>(name);
    keyName.Length = (USHORT)(wcslen(name) * sizeof(WCHAR));
    keyName.MaximumLength = keyName.Length + sizeof(WCHAR);

    PLSA_UNICODE_STRING data = nullptr;
    NTSTATUS st = LsaRetrievePrivateData(hPolicy, &keyName, &data);
    LsaClose(hPolicy);
    if (st != STATUS_SUCCESS || !data)
        return false;

    if (data->Buffer && data->Length)
    {
        out.assign((BYTE*)data->Buffer, (BYTE*)data->Buffer + data->Length);
        SecureZeroMemory(data->Buffer, data->Length);
    }
    LsaFreeMemory(data);
    return !out.empty();
}

bool PU_ReadConfig(std::wstring& sid, DWORD& port)
{
    WCHAR buf[256];
    DWORD cb = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, PU_REG_KEY, L"UserSid", RRF_RT_REG_SZ, nullptr, buf, &cb) != ERROR_SUCCESS)
        return false;
    sid = buf;

    DWORD p = 0;
    cb = sizeof(p);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, PU_REG_KEY, L"Port", RRF_RT_REG_DWORD, nullptr, &p, &cb) != ERROR_SUCCESS || p == 0 || p > 65535)
        p = PU_DEFAULT_PORT;
    port = p;
    return !sid.empty();
}

bool PU_GetFlag(const wchar_t* name)
{
    DWORD v = 0, cb = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, PU_REG_KEY, name, RRF_RT_REG_DWORD, nullptr, &v, &cb) != ERROR_SUCCESS)
        return false;
    return v != 0;
}

void PU_SetFlag(const wchar_t* name, bool value)
{
    DWORD v = value ? 1 : 0;
    RegSetKeyValueW(HKEY_LOCAL_MACHINE, PU_REG_KEY, name, REG_DWORD, &v, sizeof(v));
}

// 6-digit code both screens show during pairing, derived from the key.
DWORD PU_PairCode(const std::vector<BYTE>& key)
{
    BYTE h[32];
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)key.data(), (ULONG)key.size(), h, 32) != STATUS_SUCCESS)
        return 0;
    DWORD v = ((DWORD)h[0] << 24) | ((DWORD)h[1] << 16) | ((DWORD)h[2] << 8) | h[3];
    return v % 1000000;
}

bool PU_BtOpen(PU_BtServer& b, const GUID& uuid, const wchar_t* name)
{
    b.uuid = uuid;
    b.s = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (b.s == INVALID_SOCKET) return false;

    b.sa.addressFamily = AF_BTH;
    b.sa.port = BT_PORT_ANY;
    int len = sizeof(b.sa);
    if (bind(b.s, (sockaddr*)&b.sa, sizeof(b.sa)) != 0 ||
        getsockname(b.s, (sockaddr*)&b.sa, &len) != 0 ||
        listen(b.s, 2) != 0)
    {
        closesocket(b.s);
        b.s = INVALID_SOCKET;
        return false;
    }

    // Publish an SDP record so the phone can find the channel by UUID.
    b.csa.LocalAddr.lpSockaddr = (LPSOCKADDR)&b.sa;
    b.csa.LocalAddr.iSockaddrLength = sizeof(b.sa);
    b.csa.iSocketType = SOCK_STREAM;
    b.csa.iProtocol = BTHPROTO_RFCOMM;
    b.qs.dwSize = sizeof(b.qs);
    b.qs.lpszServiceInstanceName = const_cast<LPWSTR>(name);
    b.qs.lpServiceClassId = &b.uuid;
    b.qs.dwNameSpace = NS_BTH;
    b.qs.dwNumberOfCsAddrs = 1;
    b.qs.lpcsaBuffer = &b.csa;
    b.registered = WSASetServiceW(&b.qs, RNRSERVICE_REGISTER, 0) == 0;
    return true;
}

void PU_BtClose(PU_BtServer& b)
{
    if (b.registered) WSASetServiceW(&b.qs, RNRSERVICE_DELETE, 0);
    b.registered = false;
    if (b.s != INVALID_SOCKET) closesocket(b.s);
    b.s = INVALID_SOCKET;
}

static bool HmacSha256(const std::vector<BYTE>& key, const BYTE* data, ULONG len, BYTE mac[32])
{
    return BCryptHash(BCRYPT_HMAC_SHA256_ALG_HANDLE, (PUCHAR)key.data(), (ULONG)key.size(),
                      (PUCHAR)data, len, mac, 32) == STATUS_SUCCESS;
}

bool PU_VerifyPacket(const BYTE* p, int len, const std::vector<BYTE>& key, ULONGLONG* tsOut)
{
    if (len != PU_PACKET_LEN || p[0] != PU_PACKET_VERSION || key.size() < 16)
        return false;

    BYTE mac[32];
    if (!HmacSha256(key, p, 9, mac))
        return false;

    BYTE diff = 0;
    for (int i = 0; i < 32; i++)
        diff |= (BYTE)(mac[i] ^ p[9 + i]);
    if (diff != 0)
        return false;

    ULONGLONG ts = 0;
    for (int i = 1; i <= 8; i++)
        ts = (ts << 8) | p[i];

    ULONGLONG now = PU_NowUnixMs();
    ULONGLONG skew = now > ts ? now - ts : ts - now;
    if (skew > PU_MAX_SKEW_MS)
        return false;

    *tsOut = ts;
    return true;
}

// Replay protection: each accepted timestamp must be strictly newer than the last one.
bool PU_AcceptTimestamp(ULONGLONG ts)
{
    HKEY hk;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, PU_REG_KEY, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &hk) != ERROR_SUCCESS)
        return false;

    ULONGLONG last = 0;
    DWORD type = 0, cb = sizeof(last);
    if (RegQueryValueExW(hk, L"LastTs", nullptr, &type, (BYTE*)&last, &cb) != ERROR_SUCCESS || type != REG_QWORD)
        last = 0;

    bool ok = ts > last;
    if (ok)
        ok = RegSetValueExW(hk, L"LastTs", 0, REG_QWORD, (const BYTE*)&ts, sizeof(ts)) == ERROR_SUCCESS;
    RegCloseKey(hk);
    return ok;
}

bool PU_BuildPacket(const std::vector<BYTE>& key, BYTE out[PU_PACKET_LEN])
{
    ULONGLONG ts = PU_NowUnixMs();
    out[0] = PU_PACKET_VERSION;
    for (int i = 0; i < 8; i++)
        out[1 + i] = (BYTE)(ts >> (56 - 8 * i));
    return HmacSha256(key, out, 9, out + 9);
}
