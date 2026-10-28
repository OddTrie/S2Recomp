#pragma once
// This machine's private IPv4 networks as "a.b.c.d/prefix,...". The netplay host puts them in its EOS lobby
// (attribute LAN) and the launcher's LAN tab only lists lobbies that share one of its own networks.
// Include before <windows.h> on Windows (winsock2 must come first).
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

namespace ps2lan
{
    struct Net
    {
        uint32_t addr = 0; // host byte order
        int prefix = 0;
    };

    // 10/8, 172.16/12 and 192.168/16 only: link-local and public addresses never mean "same room".
    inline bool isPrivate(uint32_t a)
    {
        return (a >> 24) == 10 || (a >> 20) == 0xAC1 || (a >> 16) == 0xC0A8;
    }

    inline std::vector<Net> localNets()
    {
        std::vector<Net> out;
#ifdef _WIN32
        // Adapters without a default gateway (WSL / Hyper-V switches, VPN leftovers) are skipped, so two PCs
        // don't "match" on a virtual 172.x network that only exists inside each of them.
        ULONG size = 16 * 1024;
        std::vector<unsigned char> buf;
        ULONG rc = ERROR_BUFFER_OVERFLOW;
        for (int tries = 0; tries < 3 && rc == ERROR_BUFFER_OVERFLOW; ++tries)
        {
            buf.resize(size);
            rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                      nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data()), &size);
        }
        if (rc != NO_ERROR)
            return out;
        for (auto *ad = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data()); ad; ad = ad->Next)
        {
            if (ad->OperStatus != IfOperStatusUp || ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK || !ad->FirstGatewayAddress)
                continue;
            for (auto *u = ad->FirstUnicastAddress; u; u = u->Next)
            {
                if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET)
                    continue;
                const uint32_t a = ntohl(reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr)->sin_addr.s_addr);
                if (isPrivate(a) && u->OnLinkPrefixLength > 0 && u->OnLinkPrefixLength <= 32)
                    out.push_back({a, static_cast<int>(u->OnLinkPrefixLength)});
            }
        }
#else
        ifaddrs *list = nullptr;
        if (getifaddrs(&list) != 0)
            return out;
        for (ifaddrs *i = list; i; i = i->ifa_next)
        {
            if (!i->ifa_addr || !i->ifa_netmask || i->ifa_addr->sa_family != AF_INET)
                continue;
            if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK))
                continue;
            const std::string name = i->ifa_name ? i->ifa_name : "";
            if (name.rfind("docker", 0) == 0 || name.rfind("br-", 0) == 0 || name.rfind("veth", 0) == 0 || name.rfind("virbr", 0) == 0)
                continue;
            const uint32_t a = ntohl(reinterpret_cast<sockaddr_in *>(i->ifa_addr)->sin_addr.s_addr);
            const uint32_t m = ntohl(reinterpret_cast<sockaddr_in *>(i->ifa_netmask)->sin_addr.s_addr);
            int prefix = 0;
            for (uint32_t b = m; b & 0x80000000u; b <<= 1)
                ++prefix;
            if (isPrivate(a) && prefix > 0)
                out.push_back({a, prefix});
        }
        freeifaddrs(list);
#endif
        return out;
    }

    inline std::string format(const std::vector<Net> &nets)
    {
        std::string s;
        for (const Net &n : nets)
        {
            char one[24];
            std::snprintf(one, sizeof(one), "%u.%u.%u.%u/%d", n.addr >> 24, (n.addr >> 16) & 255, (n.addr >> 8) & 255, n.addr & 255, n.prefix);
            if (!s.empty())
                s += ',';
            s += one;
        }
        return s;
    }

    inline std::vector<Net> parse(const std::string &s)
    {
        std::vector<Net> out;
        size_t pos = 0;
        while (pos < s.size())
        {
            size_t end = s.find(',', pos);
            if (end == std::string::npos)
                end = s.size();
            unsigned a = 0, b = 0, c = 0, d = 0;
            int p = 0;
            if (std::sscanf(s.substr(pos, end - pos).c_str(), "%u.%u.%u.%u/%d", &a, &b, &c, &d, &p) == 5 &&
                a < 256 && b < 256 && c < 256 && d < 256 && p > 0 && p <= 32)
                out.push_back({(a << 24) | (b << 16) | (c << 8) | d, p});
            pos = end + 1;
        }
        return out;
    }

    // True when some pair of networks overlaps (compared on the shorter of the two prefixes).
    inline bool shareNetwork(const std::vector<Net> &x, const std::vector<Net> &y)
    {
        for (const Net &a : x)
            for (const Net &b : y)
            {
                const int p = a.prefix < b.prefix ? a.prefix : b.prefix;
                const uint32_t mask = p >= 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> p);
                if ((a.addr & mask) == (b.addr & mask))
                    return true;
            }
        return false;
    }
}
