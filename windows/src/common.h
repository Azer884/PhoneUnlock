// Phone Unlock - shared declarations for the credential provider DLL and the setup app.
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_NO_STATUS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2bth.h>
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>

#define SECURITY_WIN32
#include <security.h>
#include <ntsecapi.h>
#include <wincred.h>
#include <credentialprovider.h>
#include <propkey.h>
#include <shlguid.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <bcrypt.h>

#include <new>
#include <string>
#include <vector>

#define PU_VERSION_STR      L"1.0.0"

// {7B2E4A91-3C5D-4F8E-9A16-2D4C8B7E5F03}
extern const CLSID CLSID_PhoneUnlockProvider;
#define PU_CLSID_STR        L"{7B2E4A91-3C5D-4F8E-9A16-2D4C8B7E5F03}"

#define PU_REG_KEY          L"SOFTWARE\\PhoneUnlock"
#define PU_SECRET_PWD       L"L$PhoneUnlock_Pwd"
#define PU_SECRET_KEY       L"L$PhoneUnlock_Key"
#define PU_DEFAULT_PORT     47600
#define PU_KEY_LEN          32

// Bluetooth RFCOMM service the phone connects to when it is unlocked.
// {6B1F3A2E-8D4C-4B7A-9E2F-5C3D1A0B7E64}
static const GUID PU_BT_UNLOCK_UUID =
    { 0x6b1f3a2e, 0x8d4c, 0x4b7a, { 0x9e, 0x2f, 0x5c, 0x3d, 0x1a, 0x0b, 0x7e, 0x64 } };

// Bluetooth RFCOMM service used once, to hand the shared key to the phone.
// {6B1F3A2E-8D4C-4B7A-9E2F-5C3D1A0B7E65}
static const GUID PU_BT_PAIR_UUID =
    { 0x6b1f3a2e, 0x8d4c, 0x4b7a, { 0x9e, 0x2f, 0x5c, 0x3d, 0x1a, 0x0b, 0x7e, 0x65 } };

// Unlock packet: [0]=version, [1..8]=unix time in ms (big endian), [9..40]=HMAC-SHA256(key, bytes 0..8)
#define PU_PACKET_VERSION   1
#define PU_PACKET_LEN       41
#define PU_MAX_SKEW_MS      60000ULL

// Pairing: PC sends "PUP1" + key (36 bytes), phone answers 1 byte (1 = user confirmed the code).
#define PU_PAIR_MAGIC       "PUP1"
#define PU_PAIR_LEN         (4 + PU_KEY_LEN)

// Reference counting for DllCanUnloadNow (DLL only; the app has no-op versions).
void DllAddRef();
void DllRelease();

// shared.cpp
ULONGLONG PU_NowUnixMs();
bool PU_ReadSecret(const wchar_t* name, std::vector<BYTE>& out);
bool PU_ReadConfig(std::wstring& sid, DWORD& port);
bool PU_GetFlag(const wchar_t* name);
void PU_SetFlag(const wchar_t* name, bool value);
bool PU_VerifyPacket(const BYTE* pkt, int len, const std::vector<BYTE>& key, ULONGLONG* tsOut);
bool PU_AcceptTimestamp(ULONGLONG ts);
bool PU_BuildPacket(const std::vector<BYTE>& key, BYTE out[PU_PACKET_LEN]);
DWORD PU_PairCode(const std::vector<BYTE>& key);
void PU_Wipe(std::vector<BYTE>& v);
void PU_Wipe(std::wstring& s);

#define PU_FLAG_FAILED      L"Failed"   // set by the provider when Windows rejected the saved password
#define PU_FLAG_PAUSED      L"Paused"   // set by the user in the app

// Bluetooth RFCOMM server with an SDP record (shared.cpp)
struct PU_BtServer
{
    SOCKET s = INVALID_SOCKET;
    SOCKADDR_BTH sa = {};
    CSADDR_INFO csa = {};
    WSAQUERYSETW qs = {};
    GUID uuid = {};
    bool registered = false;
};
bool PU_BtOpen(PU_BtServer& b, const GUID& uuid, const wchar_t* name);
void PU_BtClose(PU_BtServer& b);
