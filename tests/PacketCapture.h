// A capture of this computer's loopback traffic for the tests, by tcpdump (MONTAGE_TEST_TCPDUMP: its path, where it
// may capture), and the UDP and TCP packets read back from the file it writes (pcap, as tcpdump writes it).
#pragma once

#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QString>
#include <QThread>

#include <cstdint>
#include <string>
#include <vector>

namespace montage::testcapture {

// One packet's ends: the transport (17 UDP, 6 TCP), its addresses' family (4 or 6) and its ports.
struct Packet {
    int protocol = 0, family = 0;
    uint16_t from = 0, to = 0;
    bool loopback = false;  // both its addresses this computer's loopback (127/8, ::1)
};

// The UDP and TCP packets of a pcap file over Ethernet, Linux's cooked captures or BSD's loopback (others: none).
inline std::vector<Packet> readPcap(const QString& path, std::string* why = nullptr) {
    std::vector<Packet> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (why) *why = "cannot read " + path.toStdString();
        return out;
    }
    const QByteArray d = f.readAll();
    if (d.size() < 24) {
        if (why) *why = "no pcap header";
        return out;
    }
    const auto byte = [&](qsizetype at) { return uint8_t(d[at]); };
    const uint32_t magic = uint32_t(byte(0)) | uint32_t(byte(1)) << 8 | uint32_t(byte(2)) << 16 | uint32_t(byte(3)) << 24;
    bool little = true;
    if (magic == 0xa1b2c3d4u || magic == 0xa1b23c4du) little = true;
    else if (magic == 0xd4c3b2a1u || magic == 0x4d3cb2a1u) little = false;
    else {
        if (why) *why = "not a pcap file";
        return out;
    }
    const auto u32 = [&](qsizetype at) {
        return little ? uint32_t(byte(at)) | uint32_t(byte(at + 1)) << 8 | uint32_t(byte(at + 2)) << 16 | uint32_t(byte(at + 3)) << 24
                      : uint32_t(byte(at + 3)) | uint32_t(byte(at + 2)) << 8 | uint32_t(byte(at + 1)) << 16 | uint32_t(byte(at)) << 24;
    };
    const auto be16 = [&](qsizetype at) { return uint16_t(byte(at) << 8 | byte(at + 1)); };
    const uint32_t link = u32(20);
    for (qsizetype at = 24; at + 16 <= d.size();) {
        const qsizetype caught = qsizetype(u32(at + 8));
        const qsizetype p = at + 16;
        at = p + caught;
        if (at > d.size()) break;
        // (where the IP header begins, and its version)
        qsizetype ip = -1;
        if (link == 1 && caught >= 14) {
            const uint16_t type = be16(p + 12);
            if (type == 0x0800 || type == 0x86DD) ip = p + 14;
        } else if (link == 113 && caught >= 16) {
            const uint16_t type = be16(p + 14);
            if (type == 0x0800 || type == 0x86DD) ip = p + 16;
        } else if (link == 276 && caught >= 20) {
            const uint16_t type = be16(p);
            if (type == 0x0800 || type == 0x86DD) ip = p + 20;
        } else if ((link == 0 || link == 108) && caught >= 4) {
            ip = p + 4;
        } else if (link == 101 || link == 12 || link == 14) {
            ip = p;
        }
        if (ip < 0 || ip >= at) continue;
        Packet k;
        qsizetype transport = -1;
        if ((byte(ip) >> 4) == 4 && ip + 20 <= at) {
            k.family = 4;
            k.protocol = byte(ip + 9);
            k.loopback = byte(ip + 12) == 127 && byte(ip + 16) == 127;
            transport = ip + qsizetype(byte(ip) & 0x0f) * 4;
        } else if ((byte(ip) >> 4) == 6 && ip + 40 <= at) {
            k.family = 6;
            k.protocol = byte(ip + 6);
            const auto one = [&](qsizetype a) {
                for (int i = 0; i < 15; ++i)
                    if (byte(a + i) != 0) return false;
                return byte(a + 15) == 1;
            };
            k.loopback = one(ip + 8) && one(ip + 24);
            transport = ip + 40;
        }
        if ((k.protocol != 17 && k.protocol != 6) || transport < 0 || transport + 4 > at) continue;
        k.from = be16(transport);
        k.to = be16(transport + 2);
        out.push_back(k);
    }
    return out;
}

// tcpdump capturing the loopback's UDP and TCP into `path` until stopped (or let go of).
class LoopbackCapture {
public:
    ~LoopbackCapture() { stop(); }
    static QString exe() { return QString::fromLocal8Bit(qgetenv("MONTAGE_TEST_TCPDUMP")); }
    bool start(const QString& path, std::string* why) {
        path_ = path;
        p_.setProcessChannelMode(QProcess::MergedChannels);
        p_.start(exe(), {"-i", "lo", "-n", "-U", "-s", "128", "-w", path, "udp or tcp"});
        if (!p_.waitForStarted(10000)) {
            if (why) *why = "tcpdump did not start: " + p_.errorString().toStdString();
            return false;
        }
        // (it is capturing once it says it is listening, or once the file has its header)
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 10000 && p_.state() == QProcess::Running) {
            p_.waitForReadyRead(100);
            said_ += p_.readAll();
            if (said_.contains("listening on") || QFile(path).size() >= 24) return true;
        }
        if (why) *why = "tcpdump is not capturing: " + said_.toStdString();
        return false;
    }
    void stop() {
        if (p_.state() == QProcess::NotRunning) return;
        // (a moment for what was sent last to be written)
        QThread::msleep(300);
        p_.terminate();
        if (!p_.waitForFinished(5000)) p_.kill();
        p_.waitForFinished(3000);
        said_ += p_.readAll();
    }
    QByteArray said() const { return said_; }

private:
    QProcess p_;
    QString path_;
    QByteArray said_;
};

}  // namespace montage::testcapture
