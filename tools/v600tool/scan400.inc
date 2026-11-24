// ---- Own 400 dpi flatbed scan (no replay) --------------------------------
//
// Sequence and layouts reverse engineered from USB captures, see
// tools/v600tool/README or the project notes. Values that depend on the scan
// window or on calibration are computed here; motor tables are constants.

void put16(std::vector<unsigned char> &v, size_t o, unsigned x)
{
    v[o] = static_cast<unsigned char>(x & 0xff);
    v[o + 1] = static_cast<unsigned char>((x >> 8) & 0xff);
}

void put32(std::vector<unsigned char> &v, size_t o, uint32_t x)
{
    for (int i = 0; i < 4; ++i)
        v[o + size_t(i)] = static_cast<unsigned char>((x >> (8 * i)) & 0xff);
}

struct Session
{
    Scanner &s;

    bool cmd(unsigned char c)
    {
        return s.write({0x1e, c}) && s.expectAck();
    }
    bool cmd(unsigned char c, const std::vector<unsigned char> &p)
    {
        return cmd(c) && s.write(p) && s.expectAck();
    }
    bool cmd1b(unsigned char c)
    {
        return s.write({0x1b, c}) && s.expectAck();
    }
    bool table(const std::vector<unsigned char> &hdr, const unsigned char *d, size_t n)
    {
        return cmd(0x84) && s.write(hdr) && s.write(d, n) && s.expectAck();
    }
    // Wait until neither "initialising" (0x02) nor "busy" (0x40) is set.
    bool waitIdle(int seconds = 60)
    {
        const bool verbose = g_verbose;
        g_verbose = false;
        unsigned char st[2];
        auto t0 = std::chrono::steady_clock::now();
        bool ok = false;
        for (;;) {
            if (!s.status(st))
                break;
            if (!(st[0] & 0x42)) {
                ok = true;
                break;
            }
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(seconds)) {
                std::fprintf(stderr, "timeout waiting for idle, status %02x %02x\n", st[0], st[1]);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        g_verbose = verbose;
        return ok;
    }
    // Start (1e 47, no ACK) and read `total` bytes in transfers of up to `chunk`.
    bool start(std::vector<unsigned char> &data, size_t total, size_t chunk)
    {
        if (!s.write({0x1e, 0x47}))
            return false;
        data.resize(total);
        size_t done = 0;
        while (done < total) {
            int got = s.readChunk(data.data() + done, std::min(chunk, total - done), 30000);
            if (got <= 0)
                return false;
            done += size_t(got);
        }
        return true;
    }
};

std::vector<unsigned char> window41(unsigned start, unsigned end, uint32_t lines,
                                    std::initializer_list<unsigned char> b8to21)
{
    std::vector<unsigned char> v(22, 0);
    put16(v, 0, start);
    put16(v, 2, end);
    put32(v, 4, lines);
    size_t o = 8;
    for (unsigned char b : b8to21)
        v[o++] = b;
    return v;
}

// Mean per channel of 16-bit RGB samples (trailer stripped by caller).
void channelMeans(const unsigned char *d, size_t samples, double out[3])
{
    double sum[3] = {0, 0, 0};
    for (size_t i = 0; i < samples; ++i)
        sum[i % 3] += d[2 * i] | (d[2 * i + 1] << 8);
    for (int c = 0; c < 3; ++c)
        out[c] = sum[c] / double(samples / 3);
}

// Colour correction matrix of the V600 (epkowa's default "cct" values). The
// scanner already delivers gamma-encoded data; Epson applies the matrix to it.
const double kCct[3][3] = {{1.2578, -0.213989, -0.0437927},
                           {-0.193893, 1.2856, -0.0916901},
                           {-0.0257874, -0.264191, 1.28999}};

bool scan400(Scanner &s, double xMm, double yMm, double wMm, double hMm, bool gray,
             const std::string &outPath)
{
    Session ses{s};
    const double dpm = 400.0 / 25.4;
    // Sensor pixels 0x67 .. 0xda7 cover the glass (3392 px at 400 dpi).
    const unsigned glassStart = 0x67, glassEnd = 0xda7;
    const unsigned xPx = unsigned(std::lround(xMm * dpm));
    const unsigned yPx = unsigned(std::lround(yMm * dpm));
    unsigned outW = unsigned(wMm * dpm) / 8 * 8; // like Epson: multiple of 8
    const unsigned outH = unsigned(hMm * dpm);
    unsigned devW = (outW + 15) / 16 * 16;
    unsigned start = glassStart + xPx;
    if (start + devW > glassEnd) {
        if (devW > glassEnd - glassStart)
            devW = glassEnd - glassStart;
        start = glassEnd - devW; // shift window left, crop on the host
    }
    const unsigned end = start + devW;
    const unsigned cropX = glassStart + xPx - start;
    outW = std::min(outW, devW - cropX);
    const unsigned colourOffset = 20; // R +20, G +10, B 0 lines at 400 dpi
    // Near the top edge there is no room for the carriage pre-feed with its
    // acceleration ramp: start at the home position and drop leading lines.
    int prefeed = int(yPx) * 4 - 376;
    const bool fromHome = prefeed < 16;
    const unsigned skip = fromHome ? yPx : 0;
    if (fromHome)
        prefeed = 0;
    const uint32_t lines = outH + colourOffset + skip;
    std::printf("window: x %u..%u (%u px, output %u from %u), lines %u, %s %d\n", start, end, devW,
                outW, cropX, lines, fromHome ? "from home, skip" : "prefeed",
                fromHome ? int(skip) : prefeed);

    // Session prelude, as Epson's driver does it.
    if (!s.write({0x1e, 0xe1}) || !s.expectAck())
        return false;
    if (!s.write({0x0b, 0x00, 0x54, 0x63, 0x00, 0x00}) || !s.expectAck())
        return false;
    if (!ses.cmd1b(0x40) || !ses.waitIdle())
        return false;

    std::vector<unsigned char> identity(256);
    for (unsigned i = 0; i < 256; ++i)
        identity[i] = static_cast<unsigned char>(i);
    for (unsigned char t : {0xfc, 0xfd, 0xfe})
        if (!ses.table({0x03, 0x00, t, 0x1f, 0x02, 0x00, 0x01, 0x00}, identity.data(), 256))
            return false;

    std::vector<unsigned char> p57(42, 0);
    put32(p57, 0, 400);
    put32(p57, 4, 400);
    put32(p57, 8, xPx);
    put32(p57, 12, yPx);
    put32(p57, 16, outW);
    put32(p57, 20, outH);
    put32(p57, 24, gray ? 0x0800 : 0x0813);
    put32(p57, 28, 0x04fe);
    p57[33] = 0x80;
    if (!ses.cmd(0x57, p57))
        return false;

    // Lamp on.
    if (!ses.cmd(0xa2, {0x0a}) || !ses.waitIdle())
        return false;

    std::vector<unsigned char> r87(10), r86(4), cal(16);
    if (!ses.cmd(0x87) || !s.write({0x00}) || !s.read(r87.data(), 10))
        return false;
    if (!s.write({0x1e, 0x86}) || !s.read(r86.data(), 4))
        return false;
    if (!ses.cmd(0x93) || !s.write({0x04}) || !s.read(cal.data(), 16))
        return false;
    // cal: gains R/G/B (LE16), 2 bytes 0, offsets R/G/B, exposure LE16
    const unsigned exposure = cal[11] | (cal[12] << 8);
    std::printf("stored calibration: gains %02x %02x %02x offsets %02x %02x %02x exposure %u\n",
                cal[0], cal[2], cal[4], cal[8], cal[9], cal[10], exposure);

    std::vector<unsigned char> exp22(12, 0);
    put16(exp22, 0, exposure);
    put16(exp22, 3, exposure);
    exp22[10] = 0x01;
    std::vector<unsigned char> expDark22(12, 0);
    put16(expDark22, 0, exposure);
    expDark22[9] = 0x01;
    expDark22[10] = 0x01;

    std::vector<unsigned char> afe31(cal.begin(), cal.begin() + 12);
    afe31[11] = 0;
    std::vector<unsigned char> p21(26, 0);
    p21[0] = 0x28;
    p21[1] = 0x05;
    const std::vector<unsigned char> cal43 = {0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0x09, 0x78, 0xec,
                                              0x79, 0xf2, 0x7a, 0, 0, 0, 0, 0, 0};
    const std::vector<unsigned char> fin43 = {0x09, 0x78, 0xec, 0x79, 0xf2, 0x7a, 0x09, 0x78, 0xec,
                                              0x79, 0xf2, 0x7a, 0, 0, 0, 0, 0, 0};
    const std::vector<unsigned char> zero42(24, 0);

    if (!ses.cmd(0x5a, {0, 0, 0, 0}) || !ses.cmd(0x11, {0x85}) || !ses.cmd(0x31, afe31) ||
        !ses.cmd(0x21, p21))
        return false;
    if (!ses.table({0x07, 0, 0, 0, 0, 0, 0x01, 0}, kTable07, sizeof kTable07))
        return false;

    std::vector<unsigned char> data;

    // Pass A: one full-width line (purpose unknown, data ignored).
    if (!ses.cmd(0x22, exp22) ||
        !ses.cmd(0x41, window41(0x10, 0xdb0, 1, {1, 1, 4, 0, 0, 0, 0, 0, 0x80, 0, 1, 0, 1, 0})) ||
        !ses.cmd(0x42, zero42) || !ses.cmd(0x43, cal43) || !ses.waitIdle() ||
        !ses.start(data, (0xdb0 - 0x10) * 3 + 8, 51200))
        return false;

    // Pass B: 16 px x 4 lines, 16 bit -> dark level estimate.
    if (!ses.waitIdle() || !ses.cmd(0x5a, {0, 0, 0, 0}) || !ses.cmd(0x22, exp22) ||
        !ses.cmd(0x41, window41(0x10, 0x20, 4, {0, 1, 4, 0, 0, 0, 0, 0, 0x80, 0, 4, 0, 1, 0})) ||
        !ses.cmd(0x42, zero42) || !ses.cmd(0x43, cal43) || !ses.waitIdle() ||
        !ses.start(data, 16 * 4 * 6 + 8, 51200))
        return false;
    double darkPre[3];
    channelMeans(data.data(), 16 * 4 * 3, darkPre);

    // Pass C: white reference, 32 lines, 16 bit.
    std::vector<unsigned char> p42(24, 0);
    for (int c = 0; c < 3; ++c) {
        put16(p42, 6 + 4 * size_t(c), unsigned(std::lround(darkPre[c])));
        put16(p42, 8 + 4 * size_t(c), unsigned(std::lround(darkPre[c])));
    }
    const std::vector<unsigned char> motorConst = {0xa5, 0x00};
    if (!ses.waitIdle() || !ses.cmd(0x5a, {0, 0, 0, 0}) || !ses.cmd(0x22, exp22) ||
        !ses.cmd(0x41, window41(start, end, 32, {0, 2, 1, 0x40, 0, 0, 0, 0, 0x80, 0x80, 32, 0, 1, 0})) ||
        !ses.cmd(0x42, p42) || !ses.cmd(0x43, cal43) ||
        !ses.table({0x05, 0, 0, 0x01, 0x02, 0x02, 0, 0}, motorConst.data(), 2) ||
        !ses.cmd(0x01, {0x14, 0, 0, 0, 0x04, 0, 0, 0, 0, 0, 0x08, 0}) || !ses.waitIdle())
        return false;
    std::vector<unsigned char> white;
    if (!ses.start(white, size_t(devW) * 3 * 2 * 32 + 8, 51200))
        return false;

    // Pass D: dark reference with lamp off, 16 lines.
    std::vector<unsigned char> dark;
    auto winD = window41(start, end, 16, {0, 1, 4, 0, 0, 0, 0, 0, 0x80, 0, 16, 0, 1, 0});
    if (!ses.waitIdle() || !ses.cmd(0xa2, {0x0b}) || !ses.cmd(0x5a, {0, 0, 0, 0}) ||
        !ses.cmd(0x22, expDark22) || !ses.cmd(0x41, winD) || !ses.cmd(0x42, zero42) ||
        !ses.cmd(0x43, cal43) || !ses.cmd(0x41, winD) || !ses.waitIdle() ||
        !ses.start(dark, size_t(devW) * 3 * 2 * 16 + 8, 51200))
        return false;
    double darkLvl[3];
    channelMeans(dark.data(), size_t(devW) * 3 * 16, darkLvl);

    // Shading: per pixel white level relative to the per-channel minimum,
    // scaled by the largest power of two that fits 16 bit.
    std::vector<double> wm(size_t(devW) * 3, 0.0);
    for (unsigned l = 0; l < 32; ++l)
        for (size_t i = 0; i < wm.size(); ++i) {
            size_t o = (size_t(l) * wm.size() + i) * 2;
            wm[i] += white[o] | (white[o + 1] << 8);
        }
    double wmin[3] = {1e9, 1e9, 1e9}, wmax[3] = {0, 0, 0};
    for (size_t i = 0; i < wm.size(); ++i) {
        wm[i] /= 32.0;
        wmin[i % 3] = std::min(wmin[i % 3], wm[i]);
        wmax[i % 3] = std::max(wmax[i % 3], wm[i]);
    }
    double range = 0;
    for (int c = 0; c < 3; ++c)
        range = std::max(range, wmax[c] - std::floor(wmin[c]));
    int k = 0;
    while (k < 3 && range * double(1 << (k + 1)) <= 65535.0)
        ++k;
    std::vector<unsigned char> shading(wm.size() * 2);
    for (size_t i = 0; i < wm.size(); ++i) {
        double v = (wm[i] - std::floor(wmin[i % 3])) * double(1 << k);
        put16(shading, 2 * i, unsigned(std::clamp(std::lround(v), 0L, 65535L)));
    }
    std::printf("dark %.0f %.0f %.0f, white min %.0f %.0f %.0f max %.0f %.0f %.0f, shading x%d\n",
                darkLvl[0], darkLvl[1], darkLvl[2], wmin[0], wmin[1], wmin[2], wmax[0], wmax[1],
                wmax[2], 1 << k);

    // Lamp on again, move carriage to the start line.
    std::vector<unsigned char> pre01(12, 0);
    put32(pre01, 0, uint32_t(prefeed));
    pre01[6] = 0xff;
    pre01[7] = 0x01;
    pre01[8] = 0xff;
    pre01[10] = 0x10;
    if (!ses.waitIdle() || !ses.cmd(0xa2, {0x0a}) || !s.write({0x1e, 0x86}) ||
        !s.read(r86.data(), 4) || !ses.cmd(0x5a, {2, 0, 0, 0}))
        return false;
    if (!fromHome &&
        (!ses.table({0x05, 0, 0, 0x01, 0x02, 0, 0x06, 0}, kMotorPrefeed400, sizeof kMotorPrefeed400) ||
         !ses.cmd(0x01, pre01) || !ses.cmd(0x05) || !ses.waitIdle()))
        return false;

    // Final pass, 8 bit.
    const unsigned lpb = 51192 / (devW * 3);
    const size_t block = size_t(lpb) * devW * 3 + 8;
    std::vector<unsigned char> fin42(24, 0);
    for (int c = 0; c < 3; ++c) {
        put16(fin42, 2 * size_t(c), unsigned(std::floor(wmin[c])));
        put16(fin42, 6 + 4 * size_t(c), unsigned(std::lround(darkLvl[c])));
        put16(fin42, 8 + 4 * size_t(c), unsigned(std::lround(darkLvl[c])));
    }
    std::vector<unsigned char> shHdr = {0x06, 0x00, 0x10, 0x01, 0x02, 0, 0, 0};
    put16(shHdr, 5, unsigned(shading.size()));
    std::vector<unsigned char> fin01(12, 0);
    if (fromHome) {
        put32(fin01, 0, lines * 4 + 128 + 392);
        fin01[4] = 0x08;
        fin01[5] = 0x02;
    } else {
        put32(fin01, 0, lines * 4 + 128);
        fin01[4] = 0x80;
    }
    fin01[6] = 0xff;
    fin01[7] = 0x01;
    fin01[8] = 0xff;
    const unsigned char b15 = static_cast<unsigned char>((5 - k) << 4);
    if (!ses.cmd(0x22, exp22) ||
        !ses.cmd(0x41, window41(start, end, lines,
                                {1, 1, 4, 0x83, 0x80, 0, 0, b15, 0x80, 0xa0,
                                 static_cast<unsigned char>(lpb), 0, 1, 0})) ||
        !ses.cmd(0x42, fin42) || !ses.cmd(0x43, fin43) ||
        !ses.table(shHdr, shading.data(), shading.size()) ||
        !ses.table({0x05, 0, 0, 0x01, 0x02, 0, 0x06, 0}, kMotorScan400, sizeof kMotorScan400) ||
        !ses.cmd(0x01, fin01) || !ses.waitIdle())
        return false;

    if (!s.write({0x1e, 0x47}))
        return false;
    const size_t lineBytes = size_t(devW) * 3;
    std::vector<unsigned char> raw;
    raw.reserve(lineBytes * lines);
    std::vector<unsigned char> buf(block);
    auto t0 = std::chrono::steady_clock::now();
    while (raw.size() < lineBytes * lines) {
        size_t want = std::min(block, (lineBytes * lines - raw.size()) + 8);
        int got = s.readChunk(buf.data(), want, 30000);
        if (got <= 8)
            return false;
        raw.insert(raw.end(), buf.begin(), buf.begin() + (got - 8));
    }
    std::printf("final pass: %u lines in %.2f s\n", lines,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    ses.waitIdle();

    // Colour line offsets and crop.
    std::ofstream out(outPath, std::ios::binary);
    out << (gray ? "P5\n" : "P6\n") << outW << " " << outH << "\n255\n";
    std::vector<unsigned char> row(size_t(outW) * (gray ? 1 : 3));
    for (unsigned y = 0; y < outH; ++y) {
        const unsigned char *r = raw.data() + size_t(skip + y + 20) * lineBytes + 3 * cropX;
        const unsigned char *g = raw.data() + size_t(skip + y + 10) * lineBytes + 3 * cropX;
        const unsigned char *b = raw.data() + size_t(skip + y) * lineBytes + 3 * cropX;
        for (unsigned x = 0; x < outW; ++x) {
            const double in[3] = {double(r[3 * x]), double(g[3 * x + 1]), double(b[3 * x + 2])};
            double c[3];
            for (int i = 0; i < 3; ++i)
                c[i] = kCct[i][0] * in[0] + kCct[i][1] * in[1] + kCct[i][2] * in[2];
            auto u8 = [](double v) {
                return static_cast<unsigned char>(std::clamp(std::lround(v), 0L, 255L));
            };
            if (gray)
                row[x] = u8(0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]);
            else {
                row[3 * x] = u8(c[0]);
                row[3 * x + 1] = u8(c[1]);
                row[3 * x + 2] = u8(c[2]);
            }
        }
        out.write(reinterpret_cast<const char *>(row.data()), std::streamsize(row.size()));
    }
    std::printf("wrote %s (%u x %u)\n", outPath.c_str(), outW, outH);
    return true;
}
