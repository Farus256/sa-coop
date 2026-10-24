#include "shared/lib/firewall/Firewall.h"

#include <windows.h>
#include <netfw.h>
#include <shellapi.h>

#include "common/lib/log/Log.h"

namespace coop::firewall {

namespace {
const CLSID kPolicyClass = {0xe2b3c97f, 0x6ae1, 0x41ac, {0x81, 0x7a, 0xf6, 0xf9, 0x21, 0x66, 0xd7, 0xdd}};  // NetFwPolicy2
const IID kPolicyIid = {0x98325047, 0xc671, 0x4174, {0x8d, 0x81, 0xde, 0xfc, 0xd3, 0xf0, 0x31, 0x86}};    // INetFwPolicy2
const IID kRuleIid = {0xaf230d27, 0xbaba, 0x4e42, {0xac, 0xed, 0xf5, 0x24, 0xf2, 0x2c, 0xfc, 0xe2}};      // INetFwRule
constexpr long kProtocolAny = 256;                                                                       // NET_FW_IP_PROTOCOL_ANY

std::wstring Expanded(const wchar_t* path) {  // a rule may name its program by %ProgramFiles%...
    wchar_t buf[MAX_PATH * 2];
    DWORD n = ExpandEnvironmentStringsW(path, buf, MAX_PATH * 2);
    return n > 0 && n <= MAX_PATH * 2 ? std::wstring(buf) : std::wstring(path);
}

// One inbound rule of the program that counts for the server (UDP or any protocol) on a profile in use now
bool Counts(INetFwRule* rule, const std::wstring& program, long profiles, NET_FW_ACTION& action) {
    BSTR app = nullptr;
    VARIANT_BOOL enabled = VARIANT_FALSE;
    NET_FW_RULE_DIRECTION dir = NET_FW_RULE_DIR_OUT;
    long ruleProfiles = 0, protocol = 0;
    bool counts = SUCCEEDED(rule->get_ApplicationName(&app)) && app && !_wcsicmp(Expanded(app).c_str(), program.c_str()) &&
                  SUCCEEDED(rule->get_Enabled(&enabled)) && enabled && SUCCEEDED(rule->get_Direction(&dir)) &&
                  dir == NET_FW_RULE_DIR_IN && SUCCEEDED(rule->get_Profiles(&ruleProfiles)) && (ruleProfiles & profiles) &&
                  SUCCEEDED(rule->get_Protocol(&protocol)) && (protocol == NET_FW_IP_PROTOCOL_UDP || protocol == kProtocolAny) &&
                  SUCCEEDED(rule->get_Action(&action));
    if (app) SysFreeString(app);
    return counts;
}
}  // namespace

State Check(const std::wstring& program) {
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    State state = State::Unknown;
    INetFwPolicy2* policy = nullptr;
    if (SUCCEEDED(CoCreateInstance(kPolicyClass, nullptr, CLSCTX_INPROC_SERVER, kPolicyIid, (void**)&policy))) {
        long profiles = 0;
        policy->get_CurrentProfileTypes(&profiles);
        bool on = false;
        for (long p : {NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC}) {
            VARIANT_BOOL enabled = VARIANT_FALSE;
            if ((profiles & p) && SUCCEEDED(policy->get_FirewallEnabled((NET_FW_PROFILE_TYPE2)p, &enabled)) && enabled) on = true;
        }
        bool read = false, blocked = false, allowed = false;
        INetFwRules* rules = nullptr;
        IUnknown* unk = nullptr;
        IEnumVARIANT* it = nullptr;
        if (on && SUCCEEDED(policy->get_Rules(&rules)) && SUCCEEDED(rules->get__NewEnum(&unk)) &&
            SUCCEEDED(unk->QueryInterface(IID_IEnumVARIANT, (void**)&it))) {
            read = true;
            VARIANT v;
            VariantInit(&v);
            while (it->Next(1, &v, nullptr) == S_OK) {
                INetFwRule* rule = nullptr;
                NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
                if (v.vt == VT_DISPATCH && v.pdispVal && SUCCEEDED(v.pdispVal->QueryInterface(kRuleIid, (void**)&rule))) {
                    if (Counts(rule, program, profiles, action)) (action == NET_FW_ACTION_BLOCK ? blocked : allowed) = true;
                    rule->Release();
                }
                VariantClear(&v);
            }
            it->Release();
        }
        if (unk) unk->Release();
        if (rules) rules->Release();
        policy->Release();
        // a block rule wins over any allow rule
        state = !on ? State::Allowed : !read ? State::Unknown : blocked ? State::Blocked : allowed ? State::Allowed : State::NoRule;
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return state;
}

bool AllowElevated(const std::wstring& program) {
    std::wstring which = L"program=\"" + program + L"\"";
    std::wstring params = L"/s /c \"netsh advfirewall firewall delete rule name=all " + which +
                          L" & netsh advfirewall firewall add rule name=\"SA-COOP server\" dir=in action=allow " + which +
                          L" enable=yes profile=any\"";
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = std::wstring(sys, n < MAX_PATH ? n : 0) + L"\\cmd.exe";
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = cmd.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    bool ok = ShellExecuteExW(&sei) != FALSE;
    DWORD err = ok ? 0 : GetLastError();
    if (SUCCEEDED(init)) CoUninitialize();
    if (!ok) LOGW("Firewall: the rule for the server was not changed (error %lu%s)", err, err == ERROR_CANCELLED ? ", refused" : "");
    return ok;
}

}  // namespace coop::firewall
