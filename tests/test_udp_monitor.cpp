// test_udp_monitor.cpp
// 獨立監控程式：接收 UDP 6-float Pose6D 封包，分析發送穩定性
// 用法: test_udp_monitor.exe [port] [duration_sec] [csv_output_path]
//   預設: port=8080, duration=10s

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")

static constexpr int kDefaultPort = 8080;
static constexpr double kDefaultDurationSec = 10.0;
static constexpr size_t kChannelCount = 6;
static constexpr size_t kExpectedPayload = sizeof(float) * kChannelCount; // 24 bytes

struct Sample {
    LARGE_INTEGER qpc;
    float values[kChannelCount];
    bool valid;       // true if valid packet received this sample
};

int main(int argc, char* argv[]) {
    int port = kDefaultPort;
    double durationSec = kDefaultDurationSec;
    const char* csvPath = nullptr;

    if (argc >= 2) { int v = atoi(argv[1]); if (v > 0 && v < 65536) port = v; }
    if (argc >= 3) { double v = atof(argv[2]); if (v > 0.0) durationSec = v; }
    if (argc >= 4) csvPath = argv[3];

    // Init Winsock
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", WSAGetLastError());
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        fprintf(stderr, "socket() failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    // Set non-blocking
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    // Bind to any interface, specified port
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "bind(:%d) failed: %d\n", port, WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // Set recv buffer
    int rcvBuf = 256 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&rcvBuf, sizeof(rcvBuf));

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const double freqInv = 1.0 / static_cast<double>(freq.QuadPart);

    printf("UDP Monitor: listening on *:%d, duration=%.1fs\n", port, durationSec);
    printf("Expected payload: %zu bytes (6 floats)\n", kExpectedPayload);
    printf("QPC frequency: %lld Hz\n", freq.QuadPart);
    fflush(stdout);

    // Pre-allocate: poll at ~2kHz for durationSec
    std::vector<Sample> samples;
    samples.reserve(static_cast<size_t>(durationSec * 2000.0));

    // Also track raw packet reception for interval analysis
    struct PacketStamp {
        LARGE_INTEGER qpc;
        int bytes;
    };
    std::vector<PacketStamp> packets;
    packets.reserve(static_cast<size_t>(durationSec * 500.0));

    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    const LONGLONG t0Ticks = t0.QuadPart;
    const LONGLONG durationTicks = static_cast<LONGLONG>(durationSec * freq.QuadPart);

    float prevValues[kChannelCount] = {0};
    bool firstChanged = true;
    unsigned long totalPackets = 0;
    unsigned long malformedPackets = 0;

    FILE* csvFile = nullptr;
    if (csvPath) {
        csvFile = fopen(csvPath, "w");
        if (csvFile) fprintf(csvFile, "elapsed_ms,v0,v1,v2,v3,v4,v5,changed\n");
    }

    while (true) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        Sample s;
        s.qpc = now;
        s.valid = false;

        // Read all pending packets
        char buf[64];
        while (true) {
            int n = recvfrom(sock, buf, sizeof(buf), 0, nullptr, nullptr);
            if (n == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) break; // no more packets
                break;
            }
            totalPackets++;
            packets.push_back({now, n});

            if (n == (int)kExpectedPayload) {
                memcpy(s.values, buf, kExpectedPayload);
                s.valid = true;
            } else {
                malformedPackets++;
                // Keep previous valid values
            }
        }

        bool anyChanged = false;
        if (s.valid) {
            if (firstChanged) {
                firstChanged = false;
                anyChanged = true;
            } else {
                for (size_t i = 0; i < kChannelCount; ++i) {
                    if (s.values[i] != prevValues[i]) { anyChanged = true; break; }
                }
            }
            for (size_t i = 0; i < kChannelCount; ++i) prevValues[i] = s.values[i];
        }

        samples.push_back(s);

        if (csvFile && s.valid) {
            double elapsed = (now.QuadPart - t0Ticks) * freqInv * 1000.0;
            fprintf(csvFile, "%.3f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%d\n",
                    elapsed,
                    (double)s.values[0], (double)s.values[1],
                    (double)s.values[2], (double)s.values[3],
                    (double)s.values[4], (double)s.values[5],
                    anyChanged ? 1 : 0);
        }

        if ((now.QuadPart - t0Ticks) >= durationTicks) break;

        // Spin ~0.5ms
        LARGE_INTEGER spinTarget = now;
        spinTarget.QuadPart += freq.QuadPart / 2000;
        while (true) {
            LARGE_INTEGER cur;
            QueryPerformanceCounter(&cur);
            if (cur.QuadPart >= spinTarget.QuadPart) break;
        }
    }

    if (csvFile) fclose(csvFile);
    closesocket(sock);
    WSACleanup();

    // === Statistics ===
    size_t validSamples = 0;
    size_t changedSamples = 0;
    bool first = true;
    float prev[kChannelCount] = {0};
    for (const auto& s : samples) {
        if (!s.valid) continue;
        validSamples++;
        if (first) { first = false; changedSamples++; }
        else {
            bool ch = false;
            for (size_t i = 0; i < kChannelCount; ++i)
                if (s.values[i] != prev[i]) { ch = true; break; }
            if (ch) changedSamples++;
        }
        for (size_t i = 0; i < kChannelCount; ++i) prev[i] = s.values[i];
    }

    double totalTimeSec = (samples.back().qpc.QuadPart - t0Ticks) * freqInv;

    printf("\n========== UDP Monitor Summary ==========\n");
    printf("Duration:          %.3f s\n", totalTimeSec);
    printf("Total packets:     %lu\n", totalPackets);
    printf("Malformed:         %lu\n", malformedPackets);
    printf("Poll samples:      %zu (%.1f Hz)\n", samples.size(),
           samples.size() / totalTimeSec);
    printf("Valid samples:     %zu (%.1f Hz)\n", validSamples,
           validSamples / totalTimeSec);
    printf("Changed samples:   %zu (%.1f Hz)\n", changedSamples,
           changedSamples / totalTimeSec);
    if (validSamples > 0) {
        printf("Unchanged ratio:   %.1f%%\n",
               100.0 * (validSamples - changedSamples) / validSamples);
    }

    // Packet interval analysis
    if (packets.size() >= 2) {
        double sumInterval = 0.0, minInterval = 1e9, maxInterval = 0.0;
        LONGLONG prevTicks = 0;
        bool firstPkt = true;
        for (const auto& p : packets) {
            if (firstPkt) { firstPkt = false; prevTicks = p.qpc.QuadPart; continue; }
            double interval = (p.qpc.QuadPart - prevTicks) * freqInv * 1000.0;
            sumInterval += interval;
            if (interval < minInterval) minInterval = interval;
            if (interval > maxInterval) maxInterval = interval;
            prevTicks = p.qpc.QuadPart;
        }
        size_t intervals = packets.size() - 1;
        double avgInterval = sumInterval / intervals;
        printf("\nPacket intervals (raw UDP):\n");
        printf("  Count:   %zu\n", intervals);
        printf("  Min:     %.3f ms\n", minInterval);
        printf("  Max:     %.3f ms\n", maxInterval);
        printf("  Avg:     %.3f ms (%.1f Hz)\n", avgInterval, 1000.0 / avgInterval);
        printf("  Jitter:  %.3f ms\n", maxInterval - minInterval);
        if (maxInterval > avgInterval * 2.0)
            printf("  ** WARNING: Max > 2x Avg - irregular! **\n");
        if (maxInterval > 5.0)
            printf("  ** WARNING: Max > 5ms - significant gap! **\n");
    }

    // Channel activity
    if (!samples.empty()) {
        printf("\nChannel activity:\n");
        for (size_t ch = 0; ch < kChannelCount; ++ch) {
            size_t chChanges = 0;
            float firstVal = 0, lastVal = 0;
            bool haveFirst = false;
            for (size_t i = 0; i < samples.size(); ++i) {
                if (!samples[i].valid) continue;
                if (!haveFirst) { firstVal = samples[i].values[ch]; haveFirst = true; }
                lastVal = samples[i].values[ch];
                if (i > 0 && samples[i-1].valid &&
                    samples[i].values[ch] != samples[i-1].values[ch]) chChanges++;
            }
            printf("  Ch%zu: %zu changes, first=%.4f last=%.4f range=%.4f\n",
                   ch, chChanges, (double)firstVal, (double)lastVal,
                   (double)(lastVal - firstVal));
        }
    }

    if (csvPath) printf("\nCSV written to: %s\n", csvPath);
    return 0;
}
