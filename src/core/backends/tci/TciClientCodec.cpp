#include "core/backends/tci/TciClientCodec.h"

#include <QMap>
#include <QtEndian>

#include <cstring>

namespace AetherSDR::tci {

// ── TciEvent accessors ─────────────────────────────────────────────────────

QString TciEvent::argString(int index, const QString& fallback) const
{
    if (index < 0 || index >= args.size()) return fallback;
    return args.at(index);
}

int TciEvent::argInt(int index, int fallback) const
{
    if (index < 0 || index >= args.size()) return fallback;
    bool ok = false;
    const int value = args.at(index).toInt(&ok);
    return ok ? value : fallback;
}

qint64 TciEvent::argLongLong(int index, qint64 fallback) const
{
    if (index < 0 || index >= args.size()) return fallback;
    bool ok = false;
    const qint64 value = args.at(index).toLongLong(&ok);
    return ok ? value : fallback;
}

bool TciEvent::argBool(int index, bool fallback) const
{
    if (index < 0 || index >= args.size()) return fallback;
    const QString token = args.at(index).trimmed().toLower();
    if (token == QLatin1String("true")  || token == QLatin1String("1")) return true;
    if (token == QLatin1String("false") || token == QLatin1String("0")) return false;
    return fallback;
}

// ── Text parsing ───────────────────────────────────────────────────────────

QVector<TciEvent> TciClientCodec::parseMessage(const QString& message)
{
    QVector<TciEvent> events;

    // Split on `;` rather than on newlines. The init burst arrives as one
    // WebSocket frame holding thirty-odd notifications with no separator but
    // the terminator, so a line-oriented reader would see a single blob and
    // parse exactly one of them.
    const QStringList segments = message.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    events.reserve(segments.size());

    for (const QString& raw : segments) {
        const QString segment = raw.trimmed();
        if (segment.isEmpty()) continue;

        TciEvent event;
        const int colon = segment.indexOf(QLatin1Char(':'));
        if (colon < 0) {
            // Argument-less notifications are real and load-bearing: `ready`
            // and `start` are how a server says the init burst is complete.
            event.verb = segment.toLower();
        } else {
            event.verb = segment.left(colon).trimmed().toLower();
            const QString argsPart = segment.mid(colon + 1);
            const QStringList pieces = argsPart.split(QLatin1Char(','));
            event.args.reserve(pieces.size());
            for (const QString& piece : pieces) event.args.append(piece.trimmed());
        }

        if (event.verb.isEmpty()) continue;
        events.append(event);
    }

    return events;
}

// ── Modes ──────────────────────────────────────────────────────────────────

QString TciClientCodec::modeFromWire(const QString& tciMode, bool* ok)
{
    // INBOUND, alias-tolerant. `cwu`/`cwl` are what the K3 bridge advertises;
    // `cw`/`cwr` are what AetherSDR's own TCI server and ExpertSDR3 emit for
    // the same two sidebands. Both spellings must land on the same place or CW
    // arrives inverted with nothing to notice it by.
    static const QMap<QString, QString> map = {
        {QStringLiteral("usb"),  QStringLiteral("USB")},
        {QStringLiteral("lsb"),  QStringLiteral("LSB")},
        {QStringLiteral("cw"),   QStringLiteral("CW")},
        {QStringLiteral("cwu"),  QStringLiteral("CW")},
        {QStringLiteral("cwr"),  QStringLiteral("CWL")},
        {QStringLiteral("cwl"),  QStringLiteral("CWL")},
        {QStringLiteral("am"),   QStringLiteral("AM")},
        {QStringLiteral("sam"),  QStringLiteral("SAM")},
        {QStringLiteral("fm"),   QStringLiteral("FM")},
        {QStringLiteral("nfm"),  QStringLiteral("NFM")},
        {QStringLiteral("digu"), QStringLiteral("DIGU")},
        {QStringLiteral("digl"), QStringLiteral("DIGL")},
        {QStringLiteral("rtty"), QStringLiteral("RTTY")},
    };

    const auto it = map.constFind(tciMode.trimmed().toLower());
    if (it == map.constEnd()) {
        if (ok) *ok = false;
        // Deliberately NOT a fallback to USB. The caller decides what an
        // unknown mode means; inventing one here would put a slice in a mode
        // the radio is not actually in.
        return {};
    }
    if (ok) *ok = true;
    return it.value();
}

QString TciClientCodec::modeToWire(const QString& aetherMode)
{
    // OUTBOUND. `cwu`/`cwl` are the explicit spellings — unambiguous to a
    // server that speaks either dialect, where bare `cw` relies on the
    // server's own default sideband.
    static const QMap<QString, QString> map = {
        {QStringLiteral("USB"),  QStringLiteral("usb")},
        {QStringLiteral("LSB"),  QStringLiteral("lsb")},
        {QStringLiteral("CW"),   QStringLiteral("cwu")},
        {QStringLiteral("CWU"),  QStringLiteral("cwu")},
        {QStringLiteral("CWL"),  QStringLiteral("cwl")},
        {QStringLiteral("AM"),   QStringLiteral("am")},
        {QStringLiteral("SAM"),  QStringLiteral("sam")},
        {QStringLiteral("FM"),   QStringLiteral("fm")},
        {QStringLiteral("DFM"),  QStringLiteral("fm")},
        {QStringLiteral("FDM"),  QStringLiteral("fm")},
        {QStringLiteral("NFM"),  QStringLiteral("nfm")},
        {QStringLiteral("DIGU"), QStringLiteral("digu")},
        {QStringLiteral("DIGL"), QStringLiteral("digl")},
        {QStringLiteral("FDV"),  QStringLiteral("digu")},
        {QStringLiteral("FDVU"), QStringLiteral("digu")},
        {QStringLiteral("FDVL"), QStringLiteral("digl")},
        {QStringLiteral("RTTY"), QStringLiteral("rtty")},
    };
    return map.value(aetherMode.trimmed().toUpper(), QStringLiteral("usb"));
}

// ── Outbound commands ──────────────────────────────────────────────────────

namespace {
inline QString boolToken(bool on)
{
    return on ? QStringLiteral("true") : QStringLiteral("false");
}
}  // namespace

QString TciClientCodec::vfoSet(int trx, int channel, qint64 hz)
{
    return QStringLiteral("vfo:%1,%2,%3;").arg(trx).arg(channel).arg(hz);
}

QString TciClientCodec::modulationSet(int trx, const QString& aetherMode)
{
    return QStringLiteral("modulation:%1,%2;").arg(trx).arg(modeToWire(aetherMode));
}

QString TciClientCodec::rxFilterBandSet(int trx, int lowHz, int highHz)
{
    return QStringLiteral("rx_filter_band:%1,%2,%3;").arg(trx).arg(lowHz).arg(highHz);
}

QString TciClientCodec::trxSet(int trx, bool transmitting)
{
    return QStringLiteral("trx:%1,%2;").arg(trx).arg(boolToken(transmitting));
}

QString TciClientCodec::splitEnableSet(int trx, bool on)
{
    return QStringLiteral("split_enable:%1,%2;").arg(trx).arg(boolToken(on));
}

QString TciClientCodec::ritEnableSet(int trx, bool on)
{
    return QStringLiteral("rit_enable:%1,%2;").arg(trx).arg(boolToken(on));
}

QString TciClientCodec::ritOffsetSet(int trx, int hz)
{
    return QStringLiteral("rit_offset:%1,%2;").arg(trx).arg(hz);
}

QString TciClientCodec::xitEnableSet(int trx, bool on)
{
    return QStringLiteral("xit_enable:%1,%2;").arg(trx).arg(boolToken(on));
}

QString TciClientCodec::driveSet(int trx, int percent)
{
    return QStringLiteral("drive:%1,%2;").arg(trx).arg(qBound(0, percent, 100));
}

QString TciClientCodec::volumeSet(int db)
{
    // TCI master volume is dB, -60..0, where -60 is silence.
    return QStringLiteral("volume:%1;").arg(qBound(-60, db, 0));
}

QString TciClientCodec::muteSet(bool on)
{
    return QStringLiteral("mute:%1;").arg(boolToken(on));
}

QString TciClientCodec::audioStart(int trx)
{
    return QStringLiteral("audio_start:%1;").arg(trx);
}

QString TciClientCodec::audioStop(int trx)
{
    return QStringLiteral("audio_stop:%1;").arg(trx);
}

// ── Binary frames ──────────────────────────────────────────────────────────

std::optional<TciStreamHeader> TciClientCodec::parseStreamHeader(const QByteArray& frame)
{
    if (frame.size() < kHeaderBytes) return std::nullopt;

    // Field-by-field little-endian reads rather than a memcpy onto the struct.
    // The struct is standard-layout and the sizes match, so a memcpy would
    // work on every machine this ships to today — and would silently produce
    // garbage on a big-endian one, because TCI fixes the wire as LE.
    const auto* raw = reinterpret_cast<const uchar*>(frame.constData());
    auto word = [raw](int index) {
        return qFromLittleEndian<quint32>(raw + index * 4);
    };

    TciStreamHeader header;
    header.receiver   = word(0);
    header.sampleRate = word(1);
    header.format     = word(2);
    header.codec      = word(3);
    header.crc        = word(4);
    header.length     = word(5);
    header.type       = word(6);
    header.channels   = word(7);

    return header;
}

QByteArray TciClientCodec::payloadToFloat32(const TciStreamHeader& header,
                                            const QByteArray& frame)
{
    if (frame.size() < kHeaderBytes) return {};

    const QByteArray payload = frame.mid(kHeaderBytes);
    const int declared = static_cast<int>(header.length);
    if (declared <= 0) return {};

    // TRUST THE SMALLER OF THE TWO. `length` is what the server says it sent;
    // the frame is what actually arrived. A truncated frame whose header still
    // claims the full count would otherwise read past the buffer.
    auto sampleCount = [&](int bytesPerSample) {
        const int available = payload.size() / bytesPerSample;
        return qMin(declared, available);
    };

    QByteArray out;

    switch (header.format) {
    case FormatFloat32: {
        const int count = sampleCount(4);
        if (count <= 0) return {};
        out.resize(count * 4);
        const auto* src = reinterpret_cast<const uchar*>(payload.constData());
        auto* dst = reinterpret_cast<float*>(out.data());
        for (int i = 0; i < count; ++i) {
            // Read as a 32-bit LE word and bit-cast, so the float never rides
            // through a host-order reinterpret on a foreign-endian machine.
            const quint32 bits = qFromLittleEndian<quint32>(src + i * 4);
            float value = 0.0f;
            std::memcpy(&value, &bits, sizeof(value));
            dst[i] = value;
        }
        break;
    }
    case FormatInt16: {
        const int count = sampleCount(2);
        if (count <= 0) return {};
        out.resize(count * 4);
        const auto* src = reinterpret_cast<const uchar*>(payload.constData());
        auto* dst = reinterpret_cast<float*>(out.data());
        for (int i = 0; i < count; ++i) {
            const auto sample = static_cast<qint16>(qFromLittleEndian<quint16>(src + i * 2));
            dst[i] = static_cast<float>(sample) / 32768.0f;
        }
        break;
    }
    case FormatInt32: {
        const int count = sampleCount(4);
        if (count <= 0) return {};
        out.resize(count * 4);
        const auto* src = reinterpret_cast<const uchar*>(payload.constData());
        auto* dst = reinterpret_cast<float*>(out.data());
        for (int i = 0; i < count; ++i) {
            const auto sample = static_cast<qint32>(qFromLittleEndian<quint32>(src + i * 4));
            dst[i] = static_cast<float>(sample) / 2147483648.0f;
        }
        break;
    }
    case FormatInt24: {
        const int count = sampleCount(3);
        if (count <= 0) return {};
        out.resize(count * 4);
        const auto* src = reinterpret_cast<const uchar*>(payload.constData());
        auto* dst = reinterpret_cast<float*>(out.data());
        for (int i = 0; i < count; ++i) {
            const uchar* p = src + i * 3;
            // Sign-extend 24 bits into 32 by placing the sample in the HIGH
            // three bytes and arithmetic-shifting back down.
            const qint32 packed = static_cast<qint32>(
                (static_cast<quint32>(p[0]) << 8) |
                (static_cast<quint32>(p[1]) << 16) |
                (static_cast<quint32>(p[2]) << 24));
            dst[i] = static_cast<float>(packed >> 8) / 8388608.0f;
        }
        break;
    }
    default:
        // Unsupported/compressed codec. Returning empty makes the caller drop
        // the frame; reinterpreting the bytes would be full-scale noise.
        return {};
    }

    return out;
}

QByteArray TciClientCodec::buildStreamFrame(quint32 type, int trx, int sampleRate,
                                            int channels, const QByteArray& float32Pcm)
{
    const int sampleCount = float32Pcm.size() / 4;

    QByteArray frame;
    frame.resize(kHeaderBytes + sampleCount * 4);
    frame.fill('\0');

    auto* raw = reinterpret_cast<uchar*>(frame.data());
    auto putWord = [raw](int index, quint32 value) {
        qToLittleEndian<quint32>(value, raw + index * 4);
    };

    putWord(0, static_cast<quint32>(qMax(0, trx)));
    putWord(1, static_cast<quint32>(qMax(0, sampleRate)));
    putWord(2, FormatFloat32);
    putWord(3, 0);  // codec: uncompressed
    putWord(4, 0);  // crc: unused
    putWord(5, static_cast<quint32>(sampleCount));
    putWord(6, type);
    putWord(7, static_cast<quint32>(qMax(1, channels)));

    if (sampleCount > 0) {
        const auto* src = reinterpret_cast<const float*>(float32Pcm.constData());
        auto* dst = raw + kHeaderBytes;
        for (int i = 0; i < sampleCount; ++i) {
            quint32 bits = 0;
            std::memcpy(&bits, src + i, sizeof(bits));
            qToLittleEndian<quint32>(bits, dst + i * 4);
        }
    }

    return frame;
}

}  // namespace AetherSDR::tci
