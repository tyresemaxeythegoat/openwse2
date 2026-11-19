// The network, offline (Phase 5 R4, `docs/RECOMP_PLAN.md` "R4 design": "Offline stubs: sockets
// and HTTP fail with their real error codes"; v1.1.5.6 imports 37 WS2_32 functions, 28 of them by
// ordinal, and 13 from WINHTTP, WININET and urlmon):
//
//   WS2_32  WSAStartup WSACleanup WSAGetLastError socket WSASocketW closesocket bind connect
//           listen accept send recv sendto recvfrom select setsockopt getsockopt ioctlsocket
//           shutdown getpeername getsockname WSAIoctl WSASendTo WSARecvFrom gethostname
//           gethostbyname gethostbyaddr getaddrinfo freeaddrinfo getnameinfo __WSAFDIsSet
//           htons htonl ntohs ntohl inet_addr inet_ntoa inet_pton inet_ntop
//   WINHTTP the 11 WinHttp* imports           WININET DeleteUrlCacheEntry
//   urlmon  URLDownloadToFileA
//
// Winsock starts (WSAStartup reports version 2.2) but no socket can be created: socket and
// WSASocketW fail with WSAENETDOWN, name lookups with WSAHOST_NOT_FOUND, and calls on a socket
// handle with WSAENOTSOCK. The byte-order and address helpers are real. WinHTTP sessions,
// connections and requests are handles, and sending a request fails with
// ERROR_WINHTTP_NAME_NOT_RESOLVED. The ordinals are WS2_32's fixed exports (ws2_32.def). Winsock's
// error is the thread's last error, as on Windows. Header-only.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>

#include "wse/recomp/cpu_state.h"
#include "wse/recomp/runtime.h"
#include "wse/recomp/shim_crt.h"
#include "wse/recomp/teb.h"

namespace wse::recomp::shim {

inline constexpr std::uint32_t kWsaeNotSock = 10038, kWsaeNetDown = 10050, kWsaNotInitialised = 10093,
                               kWsaHostNotFound = 11001, kWsaeInval = 10022, kWsaeAfNoSupport = 10047;
inline constexpr std::uint64_t kInvalidSocket = ~std::uint64_t{0};
inline constexpr std::uint32_t kSocketError = 0xFFFFFFFFU;
inline constexpr std::uint32_t kErrorWinhttpNameNotResolved = 12007, kErrorWinhttpIncorrectHandleState = 12019;
inline constexpr std::uint32_t kInetEResourceNotFound = 0x800C0005U;

class Net {
public:
    explicit Net(Runtime& rt) : rt_(rt) {}
    Net(const Net&) = delete;
    Net& operator=(const Net&) = delete;

    std::size_t install() {
        std::size_t n = 0;
        // A function exported by name and (WS2_32) by its fixed ordinal.
        auto ws = [&](const char* name, int ordinal, Runtime::ImportShim shim) {
            n += rt_.set_import("WS2_32.dll", name, shim);
            if (ordinal != 0) n += rt_.set_import("WS2_32.dll", "#" + std::to_string(ordinal), shim);
        };
        using crt_detail::get;
        using crt_detail::put;
        auto sock_error = [](std::uint32_t e) {
            return [e](CpuState& s) {
                teb::set_error(s, e);
                win64_return(s, kSocketError);
            };
        };

        // WSAStartup(wVersionRequested, LPWSADATA): WSADATA (x64) wVersion wHighVersion
        // iMaxSockets iMaxUdpDg lpVendorInfo szDescription[257] szSystemStatus[129].
        ws("WSAStartup", 115, [this](CpuState& s) {
            const std::uint64_t d = win64_arg(s, 1);
            if (d == 0) return win64_return(s, 10014);  // WSAEFAULT
            std::memset(reinterpret_cast<void*>(static_cast<std::uintptr_t>(d)), 0, 408);
            put<std::uint16_t>(d, 0x0202);
            put<std::uint16_t>(d + 2, 0x0202);
            std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(d + 16)), "WinSock 2.0", 12);
            std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(d + 16 + 257)), "Offline", 8);
            ++started_;
            win64_return(s, 0);
        });
        ws("WSACleanup", 116, [this](CpuState& s) {
            if (started_.load() == 0) {
                teb::set_error(s, kWsaNotInitialised);
                return win64_return(s, kSocketError);
            }
            --started_;
            win64_return(s, 0);
        });
        ws("WSAGetLastError", 111, [](CpuState& s) {
            win64_return(s, s.teb != 0 ? crt_detail::get<std::uint32_t>(s.teb + 0x68) : 0);
        });
        ws("socket", 23, [](CpuState& s) {
            teb::set_error(s, kWsaeNetDown);
            win64_return(s, kInvalidSocket);
        });
        ws("WSASocketW", 0, [](CpuState& s) {
            teb::set_error(s, kWsaeNetDown);
            win64_return(s, kInvalidSocket);
        });
        for (const auto& [name, ordinal] : std::initializer_list<std::pair<const char*, int>>{{"closesocket", 3}, {"bind", 2}, {"connect", 4}, {"listen", 13},
                                            {"send", 19}, {"recv", 16}, {"sendto", 20}, {"recvfrom", 17},
                                            {"setsockopt", 21}, {"getsockopt", 7}, {"ioctlsocket", 10}, {"shutdown", 22},
                                            {"getpeername", 5}, {"getsockname", 6}, {"WSAIoctl", 0}, {"WSASendTo", 0},
                                            {"WSARecvFrom", 0}}) {
            ws(name, ordinal, sock_error(kWsaeNotSock));
        }
        ws("accept", 1, [](CpuState& s) {
            teb::set_error(s, kWsaeNotSock);
            win64_return(s, kInvalidSocket);
        });
        // select(nfds, readfds, writefds, exceptfds, timeout): no socket can be in a set.
        ws("select", 18, sock_error(kWsaeInval));
        // __WSAFDIsSet(SOCKET, fd_set*): fd_set is u32 count (padded to 8) then SOCKET[64].
        ws("__WSAFDIsSet", 151, [](CpuState& s) {
            const std::uint64_t set = win64_arg(s, 1);
            const std::uint32_t count = set != 0 ? get<std::uint32_t>(set) : 0;
            for (std::uint32_t i = 0; i < count && i < 64; ++i) {
                if (get<std::uint64_t>(set + 8 + 8 * std::uint64_t{i}) == win64_arg(s, 0)) return win64_return(s, 1);
            }
            win64_return(s, 0);
        });
        // gethostname(name, namelen)
        ws("gethostname", 57, [](CpuState& s) {
            static constexpr char kName[] = "wse2-android";
            const std::uint64_t buf = win64_arg(s, 0);
            if (buf == 0 || static_cast<std::int32_t>(win64_arg(s, 1)) < static_cast<std::int32_t>(sizeof(kName))) {
                teb::set_error(s, 10014);  // WSAEFAULT
                return win64_return(s, kSocketError);
            }
            std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(buf)), kName, sizeof(kName));
            win64_return(s, 0);
        });
        for (const auto& [name, ordinal] : std::initializer_list<std::pair<const char*, int>>{{"gethostbyname", 52}, {"gethostbyaddr", 51}}) {
            ws(name, ordinal, [](CpuState& s) {
                teb::set_error(s, kWsaHostNotFound);
                win64_return(s, 0);
            });
        }
        // getaddrinfo / getnameinfo return the error (and set it).
        ws("getaddrinfo", 0, [](CpuState& s) {
            if (win64_arg(s, 3) != 0) put<std::uint64_t>(win64_arg(s, 3), 0);
            teb::set_error(s, kWsaHostNotFound);
            win64_return(s, kWsaHostNotFound);
        });
        ws("getnameinfo", 0, [](CpuState& s) {
            teb::set_error(s, kWsaHostNotFound);
            win64_return(s, kWsaHostNotFound);
        });
        ws("freeaddrinfo", 0, [](CpuState& s) { win64_return(s, 0); });
        // Byte order (x64 is little-endian; network order is big-endian).
        ws("htons", 9, [](CpuState& s) { win64_return(s, swap16(static_cast<std::uint16_t>(win64_arg(s, 0)))); });
        ws("ntohs", 15, [](CpuState& s) { win64_return(s, swap16(static_cast<std::uint16_t>(win64_arg(s, 0)))); });
        ws("htonl", 8, [](CpuState& s) { win64_return(s, swap32(static_cast<std::uint32_t>(win64_arg(s, 0)))); });
        ws("ntohl", 14, [](CpuState& s) { win64_return(s, swap32(static_cast<std::uint32_t>(win64_arg(s, 0)))); });
        // inet_addr("a.b.c.d") -> network-order address; INADDR_NONE when malformed.
        ws("inet_addr", 11, [](CpuState& s) {
            std::uint32_t a = 0;
            win64_return(s, parse_ipv4(crt_detail::guest_string(win64_arg(s, 0)), a) ? a : 0xFFFFFFFFU);
        });
        // inet_ntoa(in_addr): a per-thread static buffer, as Windows.
        ws("inet_ntoa", 12, [](CpuState& s) {
            thread_local char buf[16];
            format_ipv4(static_cast<std::uint32_t>(win64_arg(s, 0)), buf);
            win64_return(s, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(buf)));
        });
        // inet_pton(family, src, dst): 1, 0 (malformed), -1 (family not supported). IPv4 only.
        ws("inet_pton", 0, [](CpuState& s) {
            if (win64_arg(s, 0) != 2) {  // AF_INET
                teb::set_error(s, kWsaeAfNoSupport);
                return win64_return(s, kSocketError);
            }
            std::uint32_t a = 0;
            if (!parse_ipv4(crt_detail::guest_string(win64_arg(s, 1)), a)) return win64_return(s, 0);
            put<std::uint32_t>(win64_arg(s, 2), a);
            win64_return(s, 1);
        });
        // inet_ntop(family, src, dst, size) -> dst or NULL.
        ws("inet_ntop", 0, [](CpuState& s) {
            const std::uint64_t dst = win64_arg(s, 2);
            char buf[16];
            if (win64_arg(s, 0) != 2) {
                teb::set_error(s, kWsaeAfNoSupport);
                return win64_return(s, 0);
            }
            format_ipv4(get<std::uint32_t>(win64_arg(s, 1)), buf);
            if (dst == 0 || win64_arg(s, 3) <= std::strlen(buf)) {
                teb::set_error(s, kWsaeInval);
                return win64_return(s, 0);
            }
            std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(dst)), buf, std::strlen(buf) + 1);
            win64_return(s, dst);
        });

        // WinHTTP: handles that never reach a server.
        auto http = [&](const char* name, Runtime::ImportShim shim) { n += rt_.set_import("WINHTTP.dll", name, shim); };
        for (const char* name : {"WinHttpOpen", "WinHttpConnect", "WinHttpOpenRequest"}) {
            http(name, [this](CpuState& s) { win64_return(s, next_handle_ += 4); });
        }
        for (const char* name : {"WinHttpCloseHandle", "WinHttpSetOption", "WinHttpSetTimeouts"}) {
            http(name, [](CpuState& s) { win64_return(s, 1); });
        }
        http("WinHttpSendRequest", [](CpuState& s) {
            teb::set_error(s, kErrorWinhttpNameNotResolved);
            win64_return(s, 0);
        });
        for (const char* name : {"WinHttpReceiveResponse", "WinHttpQueryDataAvailable", "WinHttpReadData", "WinHttpWriteData"}) {
            http(name, [](CpuState& s) {
                teb::set_error(s, kErrorWinhttpIncorrectHandleState);
                win64_return(s, 0);
            });
        }
        n += rt_.set_import("WININET.dll", "DeleteUrlCacheEntry", [](CpuState& s) {
            teb::set_error(s, 2);  // ERROR_FILE_NOT_FOUND
            win64_return(s, 0);
        });
        n += rt_.set_import("urlmon.dll", "URLDownloadToFileA", [](CpuState& s) { win64_return(s, kInetEResourceNotFound); });
        return n;
    }

private:
    static std::uint64_t swap16(std::uint16_t v) { return static_cast<std::uint16_t>((v >> 8) | (v << 8)); }
    static std::uint64_t swap32(std::uint32_t v) {
        return ((v & 0xFFU) << 24) | ((v & 0xFF00U) << 8) | ((v >> 8) & 0xFF00U) | (v >> 24);
    }
    // Dotted decimal, four parts 0-255; the result in network order.
    static bool parse_ipv4(const std::string& t, std::uint32_t& out) {
        std::uint32_t parts[4] = {};
        int count = 0;
        std::uint32_t cur = 0;
        bool digits = false;
        for (std::size_t i = 0; i <= t.size(); ++i) {
            const char c = i < t.size() ? t[i] : '.';
            if (c >= '0' && c <= '9') {
                cur = cur * 10 + static_cast<std::uint32_t>(c - '0');
                digits = true;
                if (cur > 255) return false;
            } else if (c == '.' && digits && count < 4) {
                parts[count++] = cur;
                cur = 0;
                digits = false;
            } else {
                return false;
            }
        }
        if (count != 4) return false;
        out = parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
        return true;
    }
    static void format_ipv4(std::uint32_t a, char* buf) {
        std::snprintf(buf, 16, "%u.%u.%u.%u", a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, a >> 24);
    }

    Runtime& rt_;
    std::atomic<int> started_{0};
    std::atomic<std::uint64_t> next_handle_{0x00C00000};
};

}  // namespace wse::recomp::shim
