// Title-call HLE extensions for the web profile: everything NCAA Football 10
// (ULUS10440) imports beyond the original two-game baseline, plus the offline
// network family.
//
// The NIDs below are not guesses: they were read out of the game's own
// decrypted import table (PPSSPP-decrypted EBOOT, `.lib.stub` + NID dump) and
// the names were resolved against the PPSSPP HLE tables and pspsdk headers,
// which serve as a behavioural oracle only.  No third-party code is used.
// Anything still unresolved falls back to the log-and-return-0 stub in
// kernel.cpp, so a first run with PSPWEB_TRACE_HLE=1 doubles as verification.
//
// Offline policy: the link is down, WLAN is off, there are no adhoc peers and
// DNS never resolves, so the title takes its single-player paths instead of
// blocking on a network that does not exist.

#include "kernel.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <system_error>

namespace pspweb {
namespace {

// Error codes (firmware values, cf. PPSSPP's error table).
constexpr std::uint32_t kErrAdhocDisconnected = 0x8041070Cu; // no peers / link down
constexpr std::uint32_t kErrApctlWlanOff = 0x80410A06u;      // WLAN switch off
constexpr std::uint32_t kErrResolverNoHost = 0x80410414u;    // DNS cannot resolve
constexpr std::uint32_t kErrUnknownAlmid = 0x8002019Fu;      // no such alarm
constexpr std::uint32_t kErrBadSysParam = 0x80110103u;       // unknown system param id
constexpr std::uint32_t kErrFileNotFound = 0x80010002u;

// Microseconds between 0001-01-01 and 1970-01-01: PSP ticks are µs since year 1.
constexpr std::uint64_t kRtcMagicOffset = 62135596800000000ULL;

// A locally-administered dummy MAC for formatting/parsing helpers.
constexpr std::uint8_t kDummyMac[6] = {0x02, 0x50, 0x53, 0x50, 0x52, 0x43};

// Drive state bits: PRESENT | READY | READABLE (disc in, spun up).
constexpr std::uint32_t kUmdReadyState = 0x02u | 0x10u | 0x20u;

std::uint64_t host_tick_us() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
               std::chrono::duration_cast<std::chrono::microseconds>(now).count()) +
           kRtcMagicOffset;
}

} // namespace

void Kernel::install_net() {
    // -- sceNet: Init/Term plus the ether-address string helpers the game links.
    hle("sceNet", 0x39AF39A6u, "sceNetInit", &Kernel::return_zero);
    hle("sceNet", 0x281928A9u, "sceNetTerm", &Kernel::return_zero);
    hle("sceNet", 0x89360950u, "sceNetEtherNtostr", &Kernel::sceNetEtherNtostr);
    hle("sceNet", 0xD27961C9u, "sceNetEtherStrton", &Kernel::sceNetEtherStrton);

    // -- sceWlanDrv: switch off, dummy MAC, power off.
    hle("sceWlanDrv", 0xD7763699u, "sceWlanGetSwitchState", &Kernel::sceWlanGetSwitchState);
    hle("sceWlanDrv", 0x0C622081u, "sceWlanGetEtherAddr", &Kernel::sceWlanGetEtherAddr);
    hle("sceWlanDrv", 0x93440B11u, "sceWlanDevIsPowerOn", &Kernel::return_zero);

    // -- sceNetInet: sockets always fail (return -1); Init/Term/GetErrno succeed.
    hle("sceNetInet", 0x17943399u, "sceNetInetInit", &Kernel::return_zero);
    hle("sceNetInet", 0xA9ED66B9u, "sceNetInetTerm", &Kernel::return_zero);
    hle("sceNetInet", 0xFBABE411u, "sceNetInetGetErrno", &Kernel::return_zero);
    hle("sceNetInet", 0x8B7B220Fu, "sceNetInetSocket", &Kernel::sceNetFail);
    hle("sceNetInet", 0x8D7284EAu, "sceNetInetClose", &Kernel::sceNetFail);
    hle("sceNetInet", 0x1A33F9AEu, "sceNetInetBind", &Kernel::sceNetFail);
    hle("sceNetInet", 0x410B34AAu, "sceNetInetConnect", &Kernel::sceNetFail);
    hle("sceNetInet", 0xD10A1A7Au, "sceNetInetListen", &Kernel::sceNetFail);
    hle("sceNetInet", 0xDB094E1Bu, "sceNetInetAccept", &Kernel::sceNetFail);
    hle("sceNetInet", 0x7AA671BCu, "sceNetInetSend", &Kernel::sceNetFail);
    hle("sceNetInet", 0xCDA85C99u, "sceNetInetRecv", &Kernel::sceNetFail);
    hle("sceNetInet", 0x05038FC7u, "sceNetInetSendto", &Kernel::sceNetFail);
    hle("sceNetInet", 0xC91142E4u, "sceNetInetRecvfrom", &Kernel::sceNetFail);
    hle("sceNetInet", 0x2FE71FE7u, "sceNetInetSetsockopt", &Kernel::sceNetFail);
    hle("sceNetInet", 0x4A114C7Cu, "sceNetInetGetsockopt", &Kernel::sceNetFail);
    hle("sceNetInet", 0xFAABB1DDu, "sceNetInetPoll", &Kernel::sceNetPollNone);
    hle("sceNetInet", 0xE247B6D6u, "sceNetInetGetpeername", &Kernel::sceNetFail);
    hle("sceNetInet", 0x162E6FD5u, "sceNetInetGetsockname", &Kernel::sceNetFail);
    hle("sceNetInet", 0x4CFE4E56u, "sceNetInetShutdown", &Kernel::sceNetFail);
    hle("sceNetInet", 0x8CA3A97Eu, "sceNetInetGetPspError", &Kernel::return_zero);
    hle("sceNetInet", 0xB75D5B0Au, "sceNetInetInetAddr", &Kernel::sceNetFail);

    // -- sceNetApctl: initialized, but never connected (WLAN is off).
    hle("sceNetApctl", 0xE2F91F9Bu, "sceNetApctlInit", &Kernel::return_zero);
    hle("sceNetApctl", 0xB3EDD0ECu, "sceNetApctlTerm", &Kernel::return_zero);
    hle("sceNetApctl", 0x24FE91A1u, "sceNetApctlDisconnect", &Kernel::return_zero);
    hle("sceNetApctl", 0xCFB957C6u, "sceNetApctlConnect", &Kernel::sceNetApctlOffline);
    hle("sceNetApctl", 0x8ABADD51u, "sceNetApctlAddHandler", &Kernel::return_zero);
    hle("sceNetApctl", 0x2BEFDF23u, "sceNetApctlGetInfo", &Kernel::sceNetApctlOffline);
    hle("sceNetApctl", 0x5DEAC81Bu, "sceNetApctlGetState", &Kernel::sceNetApctlGetState);

    // -- sceNetAdhoc: initialized, zero peers.
    hle("sceNetAdhoc", 0xE1D621D7u, "sceNetAdhocInit", &Kernel::return_zero);
    hle("sceNetAdhoc", 0xA62C6F57u, "sceNetAdhocTerm", &Kernel::return_zero);
    hle("sceNetAdhoc", 0x6F92741Bu, "sceNetAdhocPdpCreate", &Kernel::sceNetAdhocNoPeers);
    hle("sceNetAdhoc", 0x7F27BB5Eu, "sceNetAdhocPdpDelete", &Kernel::return_zero);
    hle("sceNetAdhoc", 0xABED3790u, "sceNetAdhocPdpSend", &Kernel::sceNetAdhocNoPeers);
    hle("sceNetAdhoc", 0xDFE53E03u, "sceNetAdhocPdpRecv", &Kernel::sceNetAdhocNoPeers);

    // -- sceNetAdhocctl: initialized, disconnected, scans find nothing.
    hle("sceNetAdhocctl", 0xE26F226Eu, "sceNetAdhocctlInit", &Kernel::return_zero);
    hle("sceNetAdhocctl", 0x9D689E13u, "sceNetAdhocctlTerm", &Kernel::return_zero);
    hle("sceNetAdhocctl", 0x20B317A0u, "sceNetAdhocctlAddHandler", &Kernel::return_zero);
    hle("sceNetAdhocctl", 0x6402490Bu, "sceNetAdhocctlDelHandler", &Kernel::return_zero);
    hle("sceNetAdhocctl", 0x34401D65u, "sceNetAdhocctlDisconnect", &Kernel::return_zero);
    hle("sceNetAdhocctl", 0x0AD043EDu, "sceNetAdhocctlConnect", &Kernel::sceNetAdhocctlNoPeers);
    hle("sceNetAdhocctl", 0x08FFF7A0u, "sceNetAdhocctlScan", &Kernel::return_zero);
    hle("sceNetAdhocctl", 0x75ECD386u, "sceNetAdhocctlGetState", &Kernel::sceNetAdhocctlGetState);
    hle("sceNetAdhocctl", 0x81AEE1BEu, "sceNetAdhocctlGetScanInfo", &Kernel::sceNetAdhocctlNoPeers);
    hle("sceNetAdhocctl", 0x5E7F79C9u, "sceNetAdhocctlJoin", &Kernel::sceNetAdhocctlNoPeers);
    hle("sceNetAdhocctl", 0xEC0635C1u, "sceNetAdhocctlCreate", &Kernel::sceNetAdhocctlNoPeers);

    // -- sceNetResolver: works, but nothing ever resolves.
    hle("sceNetResolver", 0xF3370E61u, "sceNetResolverInit", &Kernel::return_zero);
    hle("sceNetResolver", 0x6138194Au, "sceNetResolverTerm", &Kernel::return_zero);
    hle("sceNetResolver", 0x244172AFu, "sceNetResolverCreate", &Kernel::sceNetResolverCreate);
    hle("sceNetResolver", 0x94523E09u, "sceNetResolverDelete", &Kernel::return_zero);
    hle("sceNetResolver", 0x808F6063u, "sceNetResolverStop", &Kernel::return_zero);
    hle("sceNetResolver", 0x224C5F44u, "sceNetResolverStartNtoA", &Kernel::sceNetResolverNoHost);

    // -- sceUtility dialogs the game drives: OSK name entry and Netconf.
    // -- All complete immediately as finished; savedata stays in savedata.cpp.
    hle("sceUtility", 0xF6269B82u, "sceUtilityOskInitStart", &Kernel::sceUtilityOskInitStart);
    hle("sceUtility", 0x4B85C861u, "sceUtilityOskUpdate", &Kernel::return_zero);
    hle("sceUtility", 0xF3F76017u, "sceUtilityOskGetStatus", &Kernel::sceUtilityOskGetStatus);
    hle("sceUtility", 0x3DFAEBA9u, "sceUtilityOskShutdownStart", &Kernel::sceUtilityOskShutdownStart);
    hle("sceUtility", 0x4DB1E739u, "sceUtilityNetconfInitStart", &Kernel::sceUtilityNetconfCancel);
    hle("sceUtility", 0x91E70E35u, "sceUtilityNetconfUpdate", &Kernel::return_zero);
    hle("sceUtility", 0x6332AA39u, "sceUtilityNetconfGetStatus", &Kernel::sceUtilityDialogDone);
    hle("sceUtility", 0xF88155F6u, "sceUtilityNetconfShutdownStart", &Kernel::return_zero);
    hle("sceUtility", 0x42071A83u, "sceUtilityGameSharingInitStart", &Kernel::return_zero);
    hle("sceUtility", 0xD852CDCEu, "sceUtilityGameSharingUpdate", &Kernel::return_zero);
    hle("sceUtility", 0x89317C8Fu, "sceUtilityGameSharingGetStatus", &Kernel::sceUtilityDialogDone);
    hle("sceUtility", 0xD17A0573u, "sceUtilityGameSharingShutdownStart", &Kernel::return_zero);
    hle("sceUtility", 0xA5DA2406u, "sceUtilityGetSystemParamInt", &Kernel::sceUtilityGetSystemParamInt);

    // -- sceUsb: PS3 / USB connectivity (offline no-op stubs).
    hle("sceUsb", 0xAE5DE6AFu, "sceUsbStart", &Kernel::return_zero);
    hle("sceUsb", 0xC2464FA0u, "sceUsbStop", &Kernel::return_zero);
    hle("sceUsb", 0x586DB82Cu, "sceUsbActivate", &Kernel::return_zero);
    hle("sceUsb", 0xC572A9C8u, "sceUsbDeactivate", &Kernel::return_zero);

    // -- sceRtc extras: real host-backed ticks and clocks.
    hle("sceRtc", 0x3F7AD767u, "sceRtcGetCurrentTick", &Kernel::sceRtcGetCurrentTick);
    hle("sceRtc", 0x011F03C1u, "sceRtcGetAccumulativeTime", &Kernel::sceRtcGetAccumulativeTime);
    hle("sceRtc", 0x4CFA57B0u, "sceRtcGetCurrentClock", &Kernel::sceRtcGetCurrentClock);
    hle("sceRtc", 0x27C4594Cu, "sceRtcGetTime_t", &Kernel::sceRtcGetTime_t);

    // -- sceUmdUser extras: the disc is in, spun up and readable.
    hle("sceUmdUser", 0x6B4A146Cu, "sceUmdGetDriveStat", &Kernel::sceUmdGetDriveStat);
    hle("sceUmdUser", 0x20628E6Fu, "sceUmdGetErrorStat", &Kernel::return_zero);
    hle("sceUmdUser", 0xAEE7404Du, "sceUmdRegisterUMDCallBack", &Kernel::return_zero);
    hle("sceUmdUser", 0xBD2BDE07u, "sceUmdUnRegisterUMDCallBack", &Kernel::return_zero);

    // -- ModuleMgrForUser extras: fake-but-consistent module handles, like the
    // -- existing LoadModule/StartModule pair (nothing is really loaded; every
    // -- module this profile needs is HLE).  Id 1 is the main module.
    hle("ModuleMgrForUser", 0x2E0911AAu, "sceKernelUnloadModule", &Kernel::sceKernelUnloadModuleStub);
    hle("ModuleMgrForUser", 0xD1FF982Au, "sceKernelStopModule", &Kernel::return_zero);
    hle("ModuleMgrForUser", 0xD8B73127u, "sceKernelGetModuleIdByAddress", &Kernel::sceKernelGetMainModuleId);
    hle("ModuleMgrForUser", 0xF0A26395u, "sceKernelGetModuleId", &Kernel::sceKernelGetMainModuleId);
    hle("ModuleMgrForUser", 0xB7F46618u, "sceKernelLoadModuleByID", &Kernel::sceKernelLoadModuleByID);
    hle("ModuleMgrForUser", 0x8F2DF740u, "sceKernelStopUnloadSelfModuleWithStatus",
        &Kernel::sceKernelStopUnloadSelfModuleWithStatus);

    // -- ThreadManForUser extras: scheduled alarms; attr change is a no-op;
    // -- run status reflects the real thread table.
    hle("ThreadManForUser", 0x6652B8CAu, "sceKernelSetAlarm", &Kernel::sceKernelSetAlarm);
    hle("ThreadManForUser", 0x7E65B999u, "sceKernelCancelAlarm", &Kernel::sceKernelCancelAlarm);
    hle("ThreadManForUser", 0xEA748E31u, "sceKernelChangeCurrentThreadAttr", &Kernel::return_zero);
    hle("ThreadManForUser", 0xFFC36A14u, "sceKernelReferThreadRunStatus", &Kernel::sceKernelReferThreadRunStatus);

    // -- InterruptManager: registered Vblank sub-interrupt handlers.
    hle("InterruptManager", 0xCA04A2B9u, "sceKernelRegisterSubIntrHandler", &Kernel::sceKernelRegisterSubIntrHandler);
    hle("InterruptManager", 0xD61E6961u, "sceKernelReleaseSubIntrHandler", &Kernel::sceKernelReleaseSubIntrHandler);
    hle("InterruptManager", 0xFB8E22ECu, "sceKernelEnableSubIntr", &Kernel::sceKernelEnableSubIntr);
    hle("InterruptManager", 0x8A389411u, "sceKernelDisableSubIntr", &Kernel::sceKernelDisableSubIntr);

    // -- IoFileMgrForUser extras.
    hle("IoFileMgrForUser", 0x779103A0u, "sceIoRename", &Kernel::sceIoRename);
    hle("IoFileMgrForUser", 0x54F5FB11u, "sceIoDevctl", &Kernel::sceIoDevctl);

    // -- sceFont (PGF): titles that render their own text only need the
    // -- library to open; glyph rasterization is done by the game.
    hle("sceFont", 0x67CD7617u, "sceFontNewLib", &Kernel::sceFontNewLib);
    hle("sceFont", 0x574B6FBCu, "sceFontDoneLib", &Kernel::return_zero);
    hle("sceFont", 0xA834319Du, "sceFontOpenUserFile", &Kernel::sceFontOpenOk);
    hle("sceFont", 0xBB8E7B60u, "sceFontClose", &Kernel::return_zero);
    hle("sceFont", 0xEF9E4C70u, "sceFontFindOptimumFont", &Kernel::return_zero);

    // -- Misc queries EA titles poll at boot.
    hle("sceOpenPSID", 0xC69BEBCEu, "sceOpenPSIDGetOpenPSID", &Kernel::sceOpenPSIDStub);
    hle("sceReg", 0x92E41280u, "sceRegOpenRegistry", &Kernel::sceRegStub);
    hle("sceSsl", 0x957EC536u, "sceSslInit", &Kernel::return_zero);
    hle("sceHttp", 0xAB1ABE9Fu, "sceHttpInit", &Kernel::return_zero);
    hle("sceMd5", 0x198137A0u, "sceMd5BlockInit", &Kernel::return_zero);
}

// --- handlers ---------------------------------------------------------------

void Kernel::sceNetFail(Ctx &ctx) {
    (void)ctx;
    finish(ctx, 0xFFFFFFFFu); // -1: PSP socket calls fail with -1 + errno
}

void Kernel::sceNetPollNone(Ctx &ctx) {
    (void)ctx;
    finish(ctx, 0u); // no descriptors ready, nothing to report
}

void Kernel::sceNetApctlGetState(Ctx &ctx) {
    // 0 = disconnected.
    if (ctx.gpr[4] != 0u) rt_.memory().store32(ctx.gpr[4], 0u);
    finish(ctx, 0u);
}

void Kernel::sceNetApctlOffline(Ctx &ctx) {
    (void)ctx;
    finish(ctx, kErrApctlWlanOff);
}

void Kernel::sceNetEtherNtostr(Ctx &ctx) {
    // Formats the MAC as "02:50:53:50:52:43".  The firmware returns void-ish;
    // games only read the buffer.
    if (ctx.gpr[5] != 0u) {
        char text[18];
        std::snprintf(text, sizeof text, "%02x:%02x:%02x:%02x:%02x:%02x", kDummyMac[0], kDummyMac[1],
                      kDummyMac[2], kDummyMac[3], kDummyMac[4], kDummyMac[5]);
        for (int i = 0; i < 18; ++i) rt_.memory().store8(ctx.gpr[5] + i, static_cast<std::uint8_t>(text[i]));
    }
    finish(ctx, 0u);
}

void Kernel::sceNetEtherStrton(Ctx &ctx) {
    // Parses "xx:xx:xx:xx:xx:xx" back into 6 bytes; zeroes on garbage.
    std::uint8_t mac[6] = {};
    const std::string text = ctx.gpr[4] != 0u ? read_string(ctx.gpr[4], 20) : "";
    unsigned v[6] = {};
    if (std::sscanf(text.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6)
        for (int i = 0; i < 6; ++i) mac[i] = static_cast<std::uint8_t>(v[i]);
    if (ctx.gpr[5] != 0u)
        for (int i = 0; i < 6; ++i) rt_.memory().store8(ctx.gpr[5] + i, mac[i]);
    finish(ctx, 0u);
}

void Kernel::sceWlanGetSwitchState(Ctx &ctx) {
    // 0 = switch off.
    if (ctx.gpr[4] != 0u) rt_.memory().store32(ctx.gpr[4], 0u);
    finish(ctx, 0u);
}

void Kernel::sceWlanGetEtherAddr(Ctx &ctx) {
    if (ctx.gpr[4] != 0u)
        for (int i = 0; i < 6; ++i) rt_.memory().store8(ctx.gpr[4] + i, kDummyMac[i]);
    finish(ctx, 0u);
}

void Kernel::sceNetAdhocNoPeers(Ctx &ctx) {
    // Logged once: games poll these in tight loops, so per-call logging
    // would flood the console.
    if (fallback_logged_.insert("adhoc-offline").second)
        std::cerr << "[hle] adhoc unavailable, reporting disconnected (offline stub)\n";
    finish(ctx, kErrAdhocDisconnected);
}

void Kernel::sceNetAdhocctlNoPeers(Ctx &ctx) {
    (void)ctx;
    finish(ctx, kErrAdhocDisconnected);
}

void Kernel::sceNetAdhocctlGetState(Ctx &ctx) {
    // 0 = disconnected.
    if (ctx.gpr[4] != 0u) rt_.memory().store32(ctx.gpr[4], 0u);
    finish(ctx, 0u);
}

void Kernel::sceNetResolverCreate(Ctx &ctx) {
    // Hand out resolver id 1; it resolves nothing (see StartNtoA).
    if (ctx.gpr[4] != 0u) rt_.memory().store32(ctx.gpr[4], 1u);
    finish(ctx, 0u);
}

void Kernel::sceNetResolverNoHost(Ctx &ctx) {
    (void)ctx;
    finish(ctx, kErrResolverNoHost);
}

void Kernel::sceUtilityDialogDone(Ctx &ctx) {
    (void)ctx;
    // 3 = finished.  Dialogs complete immediately on desktop/web.
    finish(ctx, 3u);
}

void Kernel::sceUtilityNetconfCancel(Ctx &ctx) {
    (void)ctx;
    finish(ctx, 0u);
}

void Kernel::sceUtilityGetSystemParamInt(Ctx &ctx) {
    // US-console defaults; id 8 (language) = English keeps text in English.
    // Ids follow pspsdk's psputility_sysparam.h (BSD-licensed header).
    std::uint32_t value = 0u;
    bool known = true;
    switch (ctx.gpr[4]) {
    case 2u: value = 1u; break;    // adhoc channel 1
    case 3u: value = 0u; break;    // WLAN powersave off
    case 4u: value = 1u; break;    // date format MMDDYYYY
    case 5u: value = 1u; break;    // 12-hour clock
    case 7u: value = 0u; break;    // daylight savings off
    case 8u: value = 1u; break;    // language: English
    case 9u: value = 1u; break;    // X is confirm (US layout)
    default: known = false; break;
    }
    if (ctx.gpr[4] == 6u) {
        // Timezone: signed minutes east of UTC.  Avoids unsigned wraparound traps.
        const std::int32_t tz = -300; // EST
        if (ctx.gpr[5] != 0u) rt_.memory().store32(ctx.gpr[5], static_cast<std::uint32_t>(tz));
        finish(ctx, 0u);
        return;
    }
    if (!known) {
        finish(ctx, kErrBadSysParam);
        return;
    }
    if (ctx.gpr[5] != 0u) rt_.memory().store32(ctx.gpr[5], value);
    finish(ctx, 0u);
}

void Kernel::sceRtcGetCurrentTick(Ctx &ctx) {
    const std::uint64_t tick = host_tick_us();
    if (ctx.gpr[4] != 0u) {
        rt_.memory().store32(ctx.gpr[4], static_cast<std::uint32_t>(tick));
        rt_.memory().store32(ctx.gpr[4] + 4u, static_cast<std::uint32_t>(tick >> 32));
    }
    finish(ctx, 0u);
}

void Kernel::sceRtcGetAccumulativeTime(Ctx &ctx) {
    // u64 return in v0/v1.
    finish64(ctx, host_tick_us());
}

void Kernel::sceRtcGetCurrentClock(Ctx &ctx) {
    // Same layout as sceRtcGetCurrentClockLocalTime, with the caller's tz
    // offset in minutes applied to the clock first.
    std::time_t now = std::time(nullptr) + static_cast<std::time_t>(ctx.gpr[5]) * 60;
    std::tm tm_{};
#if defined(_WIN32)
    gmtime_s(&tm_, &now);
#else
    gmtime_r(&now, &tm_);
#endif
    const std::uint32_t out = ctx.gpr[4];
    if (out != 0u) {
        rt_.memory().store16(out + 0u, static_cast<std::uint16_t>(tm_.tm_year + 1900));
        rt_.memory().store16(out + 2u, static_cast<std::uint16_t>(tm_.tm_mon + 1));
        rt_.memory().store16(out + 4u, static_cast<std::uint16_t>(tm_.tm_mday));
        rt_.memory().store16(out + 6u, static_cast<std::uint16_t>(tm_.tm_hour));
        rt_.memory().store16(out + 8u, static_cast<std::uint16_t>(tm_.tm_min));
        rt_.memory().store16(out + 10u, static_cast<std::uint16_t>(tm_.tm_sec));
        rt_.memory().store32(out + 12u, 0u);
    }
    finish(ctx, 0u);
}

void Kernel::sceRtcGetTime_t(Ctx &ctx) {
    // Converts the ScePspDateTime at a0 to unix seconds at a1's address.
    std::tm tm_{};
    const std::uint32_t in = ctx.gpr[4];
    if (in == 0u || ctx.gpr[5] == 0u) {
        finish(ctx, 1u);
        return;
    }
    auto load16 = [this](std::uint32_t addr) -> int {
        return static_cast<int>(rt_.memory().load32(addr & ~3u) >> ((addr & 2u) * 8) & 0xFFFFu);
    };
    tm_.tm_year = load16(in + 0u) - 1900;
    tm_.tm_mon = load16(in + 2u) - 1;
    tm_.tm_mday = load16(in + 4u);
    tm_.tm_hour = load16(in + 6u);
    tm_.tm_min = load16(in + 8u);
    tm_.tm_sec = load16(in + 10u);
    tm_.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm_);
    if (t == static_cast<std::time_t>(-1)) {
        finish(ctx, 1u);
        return;
    }
    rt_.memory().store32(ctx.gpr[5], static_cast<std::uint32_t>(t));
    finish(ctx, 0u);
}

void Kernel::sceUmdGetDriveStat(Ctx &ctx) {
    (void)ctx;
    finish(ctx, kUmdReadyState);
}

void Kernel::sceKernelUnloadModuleStub(Ctx &ctx) {
    modules_.erase(static_cast<std::int32_t>(ctx.gpr[4]));
    finish(ctx, 0u);
}

void Kernel::sceKernelGetMainModuleId(Ctx &ctx) {
    (void)ctx;
    finish(ctx, 1u); // the game itself
}

void Kernel::sceKernelLoadModuleByID(Ctx &ctx) {
    (void)ctx;
    const std::int32_t uid = new_uid();
    modules_.insert(uid);
    finish(ctx, static_cast<std::uint32_t>(uid));
}

void Kernel::sceKernelStopUnloadSelfModuleWithStatus(Ctx &ctx) {
    exit_thread(ctx, static_cast<std::int32_t>(ctx.gpr[4]), true);
}

void Kernel::sceKernelSetAlarm(Ctx &ctx) {
    Thread handler;
    handler.uid = new_uid();
    handler.name = "pspweb_alarm";
    handler.entry = ctx.gpr[5];
    handler.priority = 0;
    handler.stack_size = 0x4000u;
    handler.stack_block = allocate("stack:alarm", 1u, handler.stack_size, 0u);
    if (handler.stack_block < 0) { finish(ctx, static_cast<std::uint32_t>(handler.stack_block)); return; }
    handler.stack_top = blocks_.at(handler.stack_block).address + handler.stack_size;
    handler.gp = ctx.gpr[28];
    const auto uid = handler.uid;
    threads_[uid] = std::move(handler);
    alarms_[uid] = Alarm{uid, ctx.gpr[6], now_us() + std::max(ctx.gpr[4], 1u)};
    finish(ctx, static_cast<std::uint32_t>(uid));
}

void Kernel::sceKernelCancelAlarm(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto alarm = alarms_.find(uid);
    if (alarm == alarms_.end()) { finish(ctx, kErrUnknownAlmid); return; }
    if (auto *handler = thread(alarm->second.thread_uid); handler && handler->state == ThreadState::Dormant) {
        blocks_.erase(handler->stack_block);
        threads_.erase(handler->uid);
    }
    alarms_.erase(alarm);
    finish(ctx, 0u);
}

void Kernel::sceKernelReferThreadRunStatus(Ctx &ctx) {
    // SceKernelThreadRunStatus: size, status, currentPriority, waitType,
    // waitId, wakeupCount, ...  Status codes match ReferThreadStatus.
    const std::uint32_t out = ctx.gpr[5];
    Thread *t = thread(static_cast<std::int32_t>(ctx.gpr[4]));
    if (t == nullptr) {
        finish(ctx, 0x80020198u); // unknown thread
        return;
    }
    std::uint32_t status = 0x10u; // dormant
    if (t->state == ThreadState::Running) status = 0x01u;
    else if (t->state == ThreadState::Ready) status = 0x02u;
    else if (t->state == ThreadState::Waiting) status = 0x04u;
    else if (t->state == ThreadState::Dead) status = 0x20u;
    std::uint32_t wait_type = 0u;
    if (t->state == ThreadState::Waiting) {
        switch (t->wait) {
        case WaitType::Sleep: wait_type = 1u; break;
        case WaitType::Delay:
        case WaitType::Vblank:
        case WaitType::Io: wait_type = 2u; break;
        case WaitType::Sema: wait_type = 3u; break;
        case WaitType::EventFlag: wait_type = 4u; break;
        case WaitType::Fpl: wait_type = 7u; break;
        case WaitType::ThreadEnd: wait_type = 9u; break;
        default: break;
        }
    }
    if (out != 0u) {
        const std::uint32_t size = rt_.memory().load32(out);
        const std::uint32_t words = std::min(size / 4u, 10u);
        const std::uint32_t fields[10] = {size,
                                          status,
                                          static_cast<std::uint32_t>(t->priority),
                                          wait_type,
                                          static_cast<std::uint32_t>(t->wait_uid),
                                          t->wakeup_count,
                                          static_cast<std::uint32_t>(t->exit_status),
                                          0u,
                                          0u,
                                          0u};
        for (std::uint32_t i = 1u; i < words; ++i) rt_.memory().store32(out + i * 4u, fields[i]);
    }
    finish(ctx, 0u);
}

void Kernel::sceIoRename(Ctx &ctx) {
    const std::string from = read_string(ctx.gpr[4], 256);
    const std::string to = read_string(ctx.gpr[5], 256);
    std::error_code ec;
    std::filesystem::rename(host_path(from), host_path(to), ec);
    finish(ctx, ec ? kErrFileNotFound : 0u);
}

void Kernel::sceFontNewLib(Ctx &ctx) {
    (void)ctx;
    finish(ctx, 0x464F4E54u); // fake non-zero handle
}

void Kernel::sceFontOpenOk(Ctx &ctx) {
    (void)ctx;
    finish(ctx, 0u);
}

void Kernel::sceOpenPSIDStub(Ctx &ctx) {
    if (ctx.gpr[4] != 0u)
        for (int i = 0; i < 16; ++i) rt_.memory().store8(ctx.gpr[4] + i, 0u);
    finish(ctx, 0u);
}

void Kernel::sceRegStub(Ctx &ctx) {
    (void)ctx;
    finish(ctx, 0x80010002u); // ENOENT: no registry
}

} // namespace pspweb
