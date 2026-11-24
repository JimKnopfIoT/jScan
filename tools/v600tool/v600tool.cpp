// v600tool — experimental probe for the Epson Perfection V600 Photo (04b8:013a)
//
// Talks to the scanner directly via libusb, without Epson's non-free plugin.
// Step 1: firmware upload (if needed), wait until ready, read identity and
// scan parameters. Only commands observed in a capture of Epson's own driver
// are sent.
//
// The firmware file (esfwA1.bin) is not part of this project; it comes from
// Epson's iscan-plugin-gt-x820 package.

#include <libusb-1.0/libusb.h>

#include "../../src/v600_tables_400.h"

#include <algorithm>
#include <cmath>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint16_t kVid = 0x04b8;
constexpr uint16_t kPid = 0x013a;
constexpr unsigned char kEpOut = 0x02;
constexpr unsigned char kEpIn = 0x81;
constexpr unsigned kTimeoutMs = 5000;
constexpr unsigned char kAck = 0x06;

constexpr size_t kFwHeader = 256;
constexpr size_t kFwPayload = 65536;

bool g_verbose = false;

void hexdump(const char *dir, const unsigned char *d, size_t n)
{
    std::printf("%s %5zu:", dir, n);
    for (size_t i = 0; i < n && i < 48; ++i)
        std::printf(" %02x", d[i]);
    if (n > 48)
        std::printf(" …");
    std::printf("\n");
}

class Scanner
{
public:
    ~Scanner()
    {
        if (h_) {
            libusb_release_interface(h_, 0);
            libusb_close(h_);
        }
        if (ctx_)
            libusb_exit(ctx_);
    }

    bool open()
    {
        if (libusb_init(&ctx_) != 0)
            return false;
        h_ = libusb_open_device_with_vid_pid(ctx_, kVid, kPid);
        if (!h_) {
            std::fprintf(stderr, "V600 (04b8:013a) not found or no permission\n");
            return false;
        }
        int r = libusb_claim_interface(h_, 0);
        if (r != 0) {
            std::fprintf(stderr, "claim_interface: %s\n", libusb_error_name(r));
            return false;
        }
        return true;
    }

    bool write(const std::vector<unsigned char> &d) { return write(d.data(), d.size()); }

    bool write(const unsigned char *d, size_t n)
    {
        if (g_verbose)
            hexdump(">", d, n);
        size_t done = 0;
        while (done < n) {
            int got = 0;
            int r = libusb_bulk_transfer(h_, kEpOut, const_cast<unsigned char *>(d + done),
                                         int(n - done), &got, kTimeoutMs);
            if (r != 0) {
                std::fprintf(stderr, "bulk out: %s\n", libusb_error_name(r));
                return false;
            }
            done += size_t(got);
        }
        return true;
    }

    // Reads exactly n bytes.
    bool read(unsigned char *d, size_t n)
    {
        size_t done = 0;
        while (done < n) {
            int got = 0;
            int r = libusb_bulk_transfer(h_, kEpIn, d + done, int(n - done), &got, kTimeoutMs);
            if (r != 0) {
                std::fprintf(stderr, "bulk in: %s\n", libusb_error_name(r));
                return false;
            }
            done += size_t(got);
        }
        if (g_verbose)
            hexdump("<", d, n);
        return true;
    }

    // One bulk IN transfer of up to n bytes; returns bytes received or -1.
    int readChunk(unsigned char *d, size_t n, unsigned timeoutMs)
    {
        int got = 0;
        int r = libusb_bulk_transfer(h_, kEpIn, d, int(n), &got, timeoutMs);
        if (r != 0) {
            std::fprintf(stderr, "bulk in (data): %s\n", libusb_error_name(r));
            return -1;
        }
        return got;
    }

    std::vector<unsigned char> query(std::vector<unsigned char> cmd, size_t replyLen)
    {
        std::vector<unsigned char> reply(replyLen);
        if (!write(cmd) || !read(reply.data(), replyLen))
            reply.clear();
        return reply;
    }

    bool expectAck()
    {
        unsigned char a = 0;
        if (!read(&a, 1))
            return false;
        if (a != kAck) {
            std::fprintf(stderr, "expected ACK 06, got %02x\n", a);
            return false;
        }
        return true;
    }

    bool status(unsigned char st[2])
    {
        auto r = query({0x1b, 0x03}, 2);
        if (r.size() != 2)
            return false;
        st[0] = r[0];
        st[1] = r[1];
        return true;
    }

    bool uploadFirmware(const std::string &path)
    {
        std::ifstream f(path, std::ios::binary);
        std::vector<unsigned char> fw((std::istreambuf_iterator<char>(f)), {});
        if (fw.size() < kFwHeader + kFwPayload || std::memcmp(fw.data(), "EDLA", 4) != 0) {
            std::fprintf(stderr, "%s: not a valid EDLA firmware file\n", path.c_str());
            return false;
        }
        const unsigned char *payload = fw.data() + kFwHeader;
        unsigned char sum = 0;
        for (size_t i = 0; i < kFwPayload; ++i)
            sum = static_cast<unsigned char>(sum + payload[i]);

        std::printf("uploading firmware (%zu bytes, sum %02x)…\n", kFwPayload, sum);
        if (!write({0x1b, 0x06}) || !expectAck())
            return false;
        if (!write({0x01, 0x00, 0x01, 0x00}) || !write(payload, kFwPayload))
            return false;
        if (!write(&sum, 1) || !expectAck())
            return false;
        if (!write({0x1b, 0x16}) || !expectAck())
            return false;
        if (!write({0x80}) || !expectAck())
            return false;
        return true;
    }

private:
    libusb_context *ctx_ = nullptr;
    libusb_device_handle *h_ = nullptr;
};

void printParams(const std::vector<unsigned char> &p)
{
    auto le32 = [&](size_t o) {
        return uint32_t(p[o]) | uint32_t(p[o + 1]) << 8 | uint32_t(p[o + 2]) << 16 |
               uint32_t(p[o + 3]) << 24;
    };
    hexdump("params", p.data(), p.size());
    if (p.size() < 42)
        return;
    std::printf("  res x/y   : %u / %u dpi\n", le32(0), le32(4));
    std::printf("  offset x/y: %u / %u\n", le32(8), le32(12));
    std::printf("  size x/y  : %u / %u px\n", le32(16), le32(20));
    std::printf("  bytes 24..: ");
    for (size_t i = 24; i < p.size(); ++i)
        std::printf("%02x ", p[i]);
    std::printf("\n");
}

std::vector<unsigned char> fromHex(const std::string &h)
{
    std::vector<unsigned char> v;
    for (size_t i = 0; i + 1 < h.size(); i += 2)
        v.push_back(static_cast<unsigned char>(std::stoi(h.substr(i, 2), nullptr, 16)));
    return v;
}

// Replays a script produced by pcap2seq.py. Data phases are written to
// <outPrefix>-<n>.bin (raw, including the 8-byte block trailers).
bool replay(Scanner &s, const std::string &script, const std::string &outPrefix)
{
    std::ifstream in(script);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", script.c_str());
        return false;
    }
    std::string line;
    int lineNo = 0;
    int phase = 0;
    std::vector<unsigned char> buf;
    // Status byte 0: 0x02 = initialising/warming, 0x40 = busy (carriage moving).
    // Byte 1 0x80 meaning unknown (set in Epson's sessions); we only wait
    // until the scanner is not busy.
    while (std::getline(in, line)) {
        ++lineNo;
        std::istringstream ls(line);
        std::string op;
        ls >> op;
        if (op == "W") {
            std::string hex;
            ls >> hex;
            if (!s.write(fromHex(hex)))
                return false;
        } else if (op == "R") {
            size_t n = 0;
            ls >> n;
            std::vector<unsigned char> r(n);
            if (!s.read(r.data(), n))
                return false;
            if (n == 1 && r[0] != kAck)
                std::printf("line %d: reply %02x (not ACK)\n", lineNo, r[0]);
            else if (n > 1 && !g_verbose)
                hexdump("<", r.data(), n);
        } else if (op == "P") {
            std::string hex;
            ls >> hex;
            auto want = fromHex(hex);
            const bool verbose = g_verbose;
            g_verbose = false;
            unsigned char st[2] = {};
            auto t0 = std::chrono::steady_clock::now();
            unsigned char last[2] = {0xff, 0xff};
            for (;;) {
                if (!s.status(st))
                    return false;
                if (st[0] != last[0] || st[1] != last[1]) {
                    std::printf("line %d: status %02x %02x (%.1f s)\n", lineNo, st[0], st[1],
                                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
                    last[0] = st[0];
                    last[1] = st[1];
                }
                if (!(st[0] & 0x42))
                    break;
                if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(90)) {
                    std::printf("line %d: status %02x %02x, expected %s — continuing\n", lineNo,
                                st[0], st[1], hex.c_str());
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            g_verbose = verbose;
        } else if (op == "D") {
            size_t size = 0, count = 0;
            ls >> size >> count;
            ++phase;
            std::string path = outPrefix + "-" + std::to_string(phase) + ".bin";
            std::ofstream out(path, std::ios::binary | std::ios::app);
            buf.resize(size);
            size_t total = 0;
            auto t0 = std::chrono::steady_clock::now();
            for (size_t k = 0; k < count; ++k) {
                int got = s.readChunk(buf.data(), size, 30000);
                if (got < 0)
                    return false;
                if (size_t(got) != size)
                    std::printf("line %d: block %zu short: %d of %zu\n", lineNo, k, got, size);
                out.write(reinterpret_cast<const char *>(buf.data()), got);
                total += size_t(got);
            }
            std::printf("data phase %d: %zu x %zu -> %zu bytes in %.2f s -> %s\n", phase, count,
                        size, total,
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
                        path.c_str());
            // consecutive D lines (e.g. last shorter block) belong to the same phase
            std::streampos pos = in.tellg();
            std::string next;
            if (std::getline(in, next) && next.rfind("D ", 0) == 0)
                --phase;
            in.seekg(pos);
        } else if (!op.empty() && op[0] != '#') {
            std::fprintf(stderr, "line %d: unknown op %s\n", lineNo, op.c_str());
            return false;
        }
    }
    return true;
}

#include "scan400.inc"

} // namespace

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::string fwPath = "/usr/share/iscan/esfwA1.bin";
    bool forceFw = false;
    std::string replayScript;
    std::string outPrefix = "v600";
    std::vector<double> scanArgs;
    std::string scanOut;
    bool gray = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-v")
            g_verbose = true;
        else if (a == "--force-fw")
            forceFw = true;
        else if (a == "--fw" && i + 1 < argc)
            fwPath = argv[++i];
        else if (a == "--replay" && i + 1 < argc)
            replayScript = argv[++i];
        else if (a == "--out" && i + 1 < argc)
            outPrefix = argv[++i];
        else if (a == "--gray")
            gray = true;
        else if (a == "--scan" && i + 5 < argc) {
            for (int k = 0; k < 4; ++k)
                scanArgs.push_back(std::stod(argv[++i]));
            scanOut = argv[++i];
        }
        else {
            std::fprintf(stderr,
                         "usage: %s [-v] [--fw esfwA1.bin] [--force-fw] [--replay scan.seq --out prefix]\n"
                         "       [--scan x y w h out.pnm [--gray]]   (mm, 400 dpi)\n",
                         argv[0]);
            return 2;
        }
    }

    Scanner s;
    if (!s.open())
        return 1;

    unsigned char st[2];
    if (!s.status(st))
        return 1;
    std::printf("status: %02x %02x\n", st[0], st[1]);

    // Capture: 28 00 before the firmware upload, 1a 81 / 5a 81 afterwards.
    // Assumption: bit 0x10 of byte 0 = firmware running.
    if (!(st[0] & 0x10) || forceFw) {
        if (!s.uploadFirmware(fwPath))
            return 1;
    } else {
        std::printf("firmware already running\n");
    }

    // Status byte 0: 0x02 = initialising (after firmware upload),
    // 0x40 = busy (e.g. carriage returning home). Wait until both are clear.
    std::printf("waiting until idle…\n");
    auto t0 = std::chrono::steady_clock::now();
    const bool verbose = g_verbose;
    g_verbose = false;
    for (;;) {
        if (!s.status(st))
            return 1;
        if (!(st[0] & 0x42))
            break;
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(60)) {
            std::fprintf(stderr, "timeout, status %02x %02x\n", st[0], st[1]);
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    g_verbose = verbose;
    std::printf("ready: status %02x %02x after %.1f s\n", st[0], st[1],
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());

    auto id = s.query({0x1b, 0x13}, 28);
    if (id.empty())
        return 1;
    std::printf("identity: \"%.*s\"\n", int(id.size()), reinterpret_cast<const char *>(id.data()));

    auto e65 = s.query({0x1e, 0x65}, 1);
    auto ea1 = s.query({0x1e, 0xa1}, 1);
    if (!e65.empty())
        std::printf("1e 65 -> %02x\n", e65[0]);
    if (!ea1.empty())
        std::printf("1e a1 -> %02x\n", ea1[0]);

    auto params = s.query({0x1e, 0x53}, 42);
    if (params.empty())
        return 1;
    printParams(params);

    if (!replayScript.empty()) {
        std::printf("replaying %s…\n", replayScript.c_str());
        if (!replay(s, replayScript, outPrefix))
            return 1;
        std::printf("replay done\n");
    }
    if (!scanArgs.empty()) {
        if (!scan400(s, scanArgs[0], scanArgs[1], scanArgs[2], scanArgs[3], gray, scanOut))
            return 1;
    }
    return 0;
}
