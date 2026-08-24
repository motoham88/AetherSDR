// TCI CLIENT-direction codec — the inverse of tci_protocol_test.
//
// tci_protocol_test covers AetherSDR impersonating a radio for a logging
// program. This covers AetherSDR as a CLIENT of somebody else's TCI server,
// which is what the "tci" backend family does.
//
// The init-burst fixture below is not invented: it is the exact text captured
// from a live k3-tci-bridge fronting an Elecraft K3, including the way the
// whole burst arrives as ONE frame with no separator but the `;`.

#include "core/backends/tci/TciClientCodec.h"

#include <QByteArray>
#include <QString>
#include <QtEndian>

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace AetherSDR::tci;

namespace {

int g_failures = 0;

bool check(bool condition, const char* message)
{
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
    return false;
}

// One captured init burst, exactly as the bridge sends it: a single WebSocket
// text frame carrying thirty notifications separated only by `;`.
const char* kInitBurst =
    "protocol:ExpertSDR3,1.5;device:Elecraft K3;receive_only:false;trx_count:1;"
    "channels_count:2;vfo_limits:100000,54000000;if_limits:-9999,9999;"
    "modulations_list:lsb,usb,cwl,cwu,nfm,am,digl,digu;iq_samplerate:48000;"
    "audio_samplerate:48000;audio_stream_sample_type:float32;"
    "audio_stream_channels:2;audio_stream_samples:2048;vfo:0,0,14028970;"
    "vfo:0,1,14230000;modulation:0,cwl;rx_filter_band:0,-200,200;"
    "rx_enable:0,true;split_enable:0,false;rit_enable:0,false;"
    "xit_enable:0,false;rit_offset:0,0;trx:0,false;drive:0,30;mic_level:13;"
    "mon_volume:0;volume:0;mute:0,false;ready;start;";

const TciEvent* find(const QVector<TciEvent>& events, const char* verb)
{
    for (const TciEvent& e : events)
        if (e.verb == QLatin1String(verb)) return &e;
    return nullptr;
}

// ── text parsing ───────────────────────────────────────────────────────────

void testInitBurstParsing()
{
    const QVector<TciEvent> events = TciClientCodec::parseMessage(QString::fromLatin1(kInitBurst));

    // THE WHOLE BURST, not just the first notification. A line-oriented reader
    // sees one blob here and parses exactly one of these — which is the bug
    // this case exists to catch.
    check(events.size() == 30, "init burst should parse to 30 notifications");

    const TciEvent* device = find(events, "device");
    check(device && device->argString(0) == QLatin1String("Elecraft K3"),
          "device should carry the free-form model string");

    const TciEvent* receiveOnly = find(events, "receive_only");
    check(receiveOnly && receiveOnly->argBool(0, true) == false,
          "receive_only:false should parse as false");

    const TciEvent* limits = find(events, "vfo_limits");
    check(limits && limits->argLongLong(0) == 100000 && limits->argLongLong(1) == 54000000,
          "vfo_limits should parse both endpoints");

    // Argument-less notifications are real and load-bearing — `ready` is how a
    // server says the capability declarations are complete.
    check(find(events, "ready") != nullptr, "bare `ready` should parse as a verb");
    check(find(events, "start") != nullptr, "bare `start` should parse as a verb");

    const TciEvent* vfo = find(events, "vfo");
    check(vfo && vfo->argLongLong(2) == 14028970,
          "vfo should carry trx, channel and Hz");

    const TciEvent* filter = find(events, "rx_filter_band");
    check(filter && filter->argInt(1) == -200 && filter->argInt(2) == 200,
          "rx_filter_band should parse a NEGATIVE low edge");
}

void testMalformedInputIsSurvivable()
{
    // A client that fails on an unknown or malformed verb cannot survive a
    // server that grows one, so every one of these must be tolerated.
    check(TciClientCodec::parseMessage(QString()).isEmpty(),
          "empty message yields no events");
    check(TciClientCodec::parseMessage(QStringLiteral(";;;")).isEmpty(),
          "a message of bare separators yields no events");

    const auto stray = TciClientCodec::parseMessage(QStringLiteral("vfo:;"));
    check(stray.size() == 1 && stray.at(0).verb == QLatin1String("vfo"),
          "a verb with an empty argument list still parses");

    const auto weird = TciClientCodec::parseMessage(
        QStringLiteral("unknown_verb:1,2,3;vfo:0,0,7000000;"));
    check(weird.size() == 2, "an unknown verb does not stop the ones after it");

    // Non-numeric arguments must yield the caller's fallback, never a 0 that
    // reads as a real frequency or a real trx index.
    const auto junk = TciClientCodec::parseMessage(QStringLiteral("vfo:a,b,c;"));
    check(junk.size() == 1 && junk.at(0).argInt(0, -1) == -1,
          "an unparseable int yields the fallback, not 0");
    check(junk.at(0).argLongLong(2, -7) == -7,
          "an unparseable long long yields the fallback");
    check(junk.at(0).argBool(1, true) == true,
          "an unparseable bool yields the fallback");
    check(junk.at(0).argInt(99, -1) == -1,
          "an out-of-range argument index yields the fallback");

    // Case and whitespace: servers differ, and the spec's own spellings are
    // uppercase while the wire is lowercase.
    const auto spaced = TciClientCodec::parseMessage(QStringLiteral("  VFO : 0 , 0 , 7000000 ;"));
    check(spaced.size() == 1 && spaced.at(0).verb == QLatin1String("vfo"),
          "verbs are matched case-insensitively and trimmed");
    check(spaced.at(0).argLongLong(2) == 7000000,
          "arguments are trimmed before parsing");
}

// ── modes ──────────────────────────────────────────────────────────────────

void testModeMapping()
{
    bool ok = false;

    // THE SIDEBAND TRAP. The K3 bridge advertises cwl/cwu; AetherSDR's own TCI
    // server emits cw/cwr for the same two sidebands. Both spellings must land
    // on the same place, or CW arrives inverted with no error anywhere.
    check(TciClientCodec::modeFromWire(QStringLiteral("cwu"), &ok) == QLatin1String("CW") && ok,
          "cwu is upper-sideband CW");
    check(TciClientCodec::modeFromWire(QStringLiteral("cw"), &ok) == QLatin1String("CW") && ok,
          "bare cw aliases to upper-sideband CW");
    check(TciClientCodec::modeFromWire(QStringLiteral("cwl"), &ok) == QLatin1String("CWL") && ok,
          "cwl is lower-sideband CW");
    check(TciClientCodec::modeFromWire(QStringLiteral("cwr"), &ok) == QLatin1String("CWL") && ok,
          "cwr aliases to lower-sideband CW");

    check(TciClientCodec::modeFromWire(QStringLiteral("LSB"), &ok) == QLatin1String("LSB") && ok,
          "inbound mode matching is case-insensitive");

    // An unknown mode must NOT silently become USB: that would put the slice in
    // a mode the radio is not actually in.
    const QString unknown = TciClientCodec::modeFromWire(QStringLiteral("zz"), &ok);
    check(!ok && unknown.isEmpty(), "an unmapped wire mode reports failure and returns empty");

    // Outbound uses the explicit sideband spellings so a server that speaks
    // either dialect cannot pick its own default.
    check(TciClientCodec::modeToWire(QStringLiteral("CW")) == QLatin1String("cwu"),
          "CW goes out as cwu");
    check(TciClientCodec::modeToWire(QStringLiteral("CWL")) == QLatin1String("cwl"),
          "CWL goes out as cwl");

    // Round-trip every mode the bridge advertises.
    const char* wireModes[] = {"lsb", "usb", "cwl", "cwu", "nfm", "am", "digl", "digu"};
    for (const char* wire : wireModes) {
        bool roundOk = false;
        const QString neutral = TciClientCodec::modeFromWire(QString::fromLatin1(wire), &roundOk);
        check(roundOk, "advertised mode must map inbound");
        check(TciClientCodec::modeToWire(neutral) == QLatin1String(wire),
              "every advertised mode round-trips to its own spelling");
    }
}

// ── command formatting ─────────────────────────────────────────────────────

void testCommandFormatting()
{
    check(TciClientCodec::vfoSet(0, 0, 14028970) == QLatin1String("vfo:0,0,14028970;"),
          "vfo command format");
    check(TciClientCodec::modulationSet(0, QStringLiteral("CWL"))
              == QLatin1String("modulation:0,cwl;"),
          "modulation command uses the wire spelling");
    check(TciClientCodec::rxFilterBandSet(0, -200, 200)
              == QLatin1String("rx_filter_band:0,-200,200;"),
          "filter command carries a negative low edge");
    check(TciClientCodec::trxSet(0, true) == QLatin1String("trx:0,true;"),
          "trx command uses true/false, not 1/0");
    check(TciClientCodec::audioStart(0) == QLatin1String("audio_start:0;"),
          "audio_start names the receiver");

    // Clamping, so an out-of-range request never reaches the radio.
    check(TciClientCodec::driveSet(0, 500) == QLatin1String("drive:0,100;"),
          "drive is clamped to 100");
    check(TciClientCodec::driveSet(0, -5) == QLatin1String("drive:0,0;"),
          "drive is clamped at 0");
    check(TciClientCodec::volumeSet(-999) == QLatin1String("volume:-60;"),
          "volume is clamped to the -60 dB floor");
    check(TciClientCodec::volumeSet(50) == QLatin1String("volume:0;"),
          "volume is clamped to the 0 dB ceiling");
}

// ── binary frames ──────────────────────────────────────────────────────────

// Builds a frame with the header the live bridge actually sends.
QByteArray makeFrame(quint32 type, quint32 format, quint32 channels,
                     quint32 declaredSamples, const QByteArray& payload,
                     quint32 receiver = 0, quint32 rate = 48000)
{
    QByteArray frame(TciClientCodec::kHeaderBytes, '\0');
    auto* raw = reinterpret_cast<uchar*>(frame.data());
    auto put = [raw](int i, quint32 v) { qToLittleEndian<quint32>(v, raw + i * 4); };
    put(0, receiver);
    put(1, rate);
    put(2, format);
    put(3, 0);
    put(4, 0);
    put(5, declaredSamples);
    put(6, type);
    put(7, channels);
    frame.append(payload);
    return frame;
}

QByteArray floatPayload(const QVector<float>& samples)
{
    QByteArray out(samples.size() * 4, '\0');
    auto* raw = reinterpret_cast<uchar*>(out.data());
    for (int i = 0; i < samples.size(); ++i) {
        quint32 bits = 0;
        std::memcpy(&bits, &samples[i], sizeof(bits));
        qToLittleEndian<quint32>(bits, raw + i * 4);
    }
    return out;
}

void testStreamHeaderParsing()
{
    // The exact header shape captured from the live bridge: 48 kHz, float32,
    // stereo, 4096 total interleaved samples in a 16448-byte frame.
    const QByteArray payload(16384, '\0');
    const QByteArray frame = makeFrame(StreamRxAudio, FormatFloat32, 2, 4096, payload);
    check(frame.size() == 16448, "the fixture matches the observed frame size");

    const auto header = TciClientCodec::parseStreamHeader(frame);
    check(header.has_value(), "a well-formed frame parses");
    if (header) {
        check(header->receiver == 0, "receiver decodes");
        check(header->sampleRate == 48000, "sample rate decodes");
        check(header->format == FormatFloat32, "format decodes");
        check(header->length == 4096, "length decodes as TOTAL interleaved samples");
        check(header->type == StreamRxAudio, "type decodes");
        check(header->channels == 2, "channel count decodes");
    }

    // A frame too short to hold a header must be refused outright rather than
    // parsed from whatever bytes happen to be there.
    check(!TciClientCodec::parseStreamHeader(QByteArray(63, '\0')).has_value(),
          "a sub-header-length frame is refused");
    check(!TciClientCodec::parseStreamHeader(QByteArray()).has_value(),
          "an empty frame is refused");

    // A header with no payload is still a valid header.
    check(TciClientCodec::parseStreamHeader(QByteArray(64, '\0')).has_value(),
          "a header-only frame parses");
}

void testFloat32Payload()
{
    const QVector<float> samples = {0.01114f, 0.01114f, -0.5f, -0.5f, 1.0f, 1.0f};
    const QByteArray frame =
        makeFrame(StreamRxAudio, FormatFloat32, 2, samples.size(), floatPayload(samples));
    const auto header = TciClientCodec::parseStreamHeader(frame);
    check(header.has_value(), "float32 fixture header parses");
    if (!header) return;

    const QByteArray pcm = TciClientCodec::payloadToFloat32(*header, frame);
    check(pcm.size() == samples.size() * 4, "float32 passes through at the same length");

    const auto* out = reinterpret_cast<const float*>(pcm.constData());
    for (int i = 0; i < samples.size(); ++i)
        check(std::fabs(out[i] - samples[i]) < 1e-6f, "float32 samples survive intact");
}

void testTruncatedPayloadIsClamped()
{
    // THE OVERREAD CASE. The header claims 4096 samples; only 8 arrived. A
    // decoder that trusts `length` reads 16 kB past the end of the buffer.
    const QVector<float> samples = {0.1f, 0.1f, 0.2f, 0.2f, 0.3f, 0.3f, 0.4f, 0.4f};
    const QByteArray frame =
        makeFrame(StreamRxAudio, FormatFloat32, 2, 4096, floatPayload(samples));

    const auto header = TciClientCodec::parseStreamHeader(frame);
    check(header.has_value(), "truncated fixture header parses");
    if (!header) return;

    const QByteArray pcm = TciClientCodec::payloadToFloat32(*header, frame);
    check(pcm.size() == samples.size() * 4,
          "decode is clamped to what ARRIVED, not to what the header claimed");
}

void testIntegerFormats()
{
    // int16: full scale and the negative rail.
    {
        QByteArray payload(3 * 2, '\0');
        auto* raw = reinterpret_cast<uchar*>(payload.data());
        qToLittleEndian<quint16>(static_cast<quint16>(16384), raw);       //  0.5
        qToLittleEndian<quint16>(static_cast<quint16>(quint16(-32768)), raw + 2);  // -1.0
        qToLittleEndian<quint16>(static_cast<quint16>(0), raw + 4);       //  0.0
        const QByteArray frame = makeFrame(StreamRxAudio, FormatInt16, 1, 3, payload);
        const auto header = TciClientCodec::parseStreamHeader(frame);
        const QByteArray pcm = TciClientCodec::payloadToFloat32(*header, frame);
        check(pcm.size() == 12, "int16 expands to float32");
        const auto* out = reinterpret_cast<const float*>(pcm.constData());
        check(std::fabs(out[0] - 0.5f) < 1e-6f, "int16 half scale");
        check(std::fabs(out[1] + 1.0f) < 1e-6f, "int16 negative full scale");
        check(std::fabs(out[2]) < 1e-6f, "int16 zero");
    }

    // int24: the sign-extension case, which is where a 3-byte format goes
    // wrong — a naive decode turns -1 into +16777215.
    {
        QByteArray payload(2 * 3, '\0');
        auto* raw = reinterpret_cast<uchar*>(payload.data());
        raw[0] = 0xFF; raw[1] = 0xFF; raw[2] = 0xFF;   // -1
        raw[3] = 0x00; raw[4] = 0x00; raw[5] = 0x40;   // +0.5 full scale
        const QByteArray frame = makeFrame(StreamRxAudio, FormatInt24, 1, 2, payload);
        const auto header = TciClientCodec::parseStreamHeader(frame);
        const QByteArray pcm = TciClientCodec::payloadToFloat32(*header, frame);
        check(pcm.size() == 8, "int24 expands to float32");
        const auto* out = reinterpret_cast<const float*>(pcm.constData());
        check(out[0] < 0.0f && std::fabs(out[0]) < 1e-5f,
              "int24 0xFFFFFF sign-extends to a small NEGATIVE value, not full scale");
        check(std::fabs(out[1] - 0.5f) < 1e-6f, "int24 half scale");
    }

    // int32.
    {
        QByteArray payload(2 * 4, '\0');
        auto* raw = reinterpret_cast<uchar*>(payload.data());
        qToLittleEndian<quint32>(static_cast<quint32>(1073741824), raw);          //  0.5
        qToLittleEndian<quint32>(static_cast<quint32>(quint32(2147483648u)), raw + 4);  // -1.0
        const QByteArray frame = makeFrame(StreamRxAudio, FormatInt32, 1, 2, payload);
        const auto header = TciClientCodec::parseStreamHeader(frame);
        const QByteArray pcm = TciClientCodec::payloadToFloat32(*header, frame);
        const auto* out = reinterpret_cast<const float*>(pcm.constData());
        check(std::fabs(out[0] - 0.5f) < 1e-6f, "int32 half scale");
        check(std::fabs(out[1] + 1.0f) < 1e-6f, "int32 negative full scale");
    }

    // An unsupported format must yield NOTHING. Reinterpreting the bytes would
    // be full-scale noise into the operator's ears.
    {
        const QByteArray frame = makeFrame(StreamRxAudio, 99, 2, 4, QByteArray(16, '\x7f'));
        const auto header = TciClientCodec::parseStreamHeader(frame);
        check(TciClientCodec::payloadToFloat32(*header, frame).isEmpty(),
              "an unsupported sample format decodes to nothing, not to noise");
    }
}

void testOutboundFrameRoundTrip()
{
    const QVector<float> samples = {0.25f, -0.25f, 0.75f, -0.75f};
    const QByteArray frame = TciClientCodec::buildStreamFrame(
        StreamTxAudio, 0, 48000, 2, floatPayload(samples));

    check(frame.size() == TciClientCodec::kHeaderBytes + samples.size() * 4,
          "built frame is header plus payload");

    const auto header = TciClientCodec::parseStreamHeader(frame);
    check(header.has_value(), "a built frame parses back");
    if (!header) return;
    check(header->type == StreamTxAudio, "built frame carries the requested type");
    check(header->format == FormatFloat32, "built frames are float32");
    check(header->sampleRate == 48000, "built frame carries the rate");
    check(header->channels == 2, "built frame carries the channel count");
    check(header->length == static_cast<quint32>(samples.size()),
          "built frame length counts total interleaved samples");

    const QByteArray pcm = TciClientCodec::payloadToFloat32(*header, frame);
    const auto* out = reinterpret_cast<const float*>(pcm.constData());
    for (int i = 0; i < samples.size(); ++i)
        check(std::fabs(out[i] - samples[i]) < 1e-6f, "built frame payload round-trips");

    // A header-only frame is legal — TX_CHRONO is exactly that.
    const QByteArray chrono =
        TciClientCodec::buildStreamFrame(StreamTxChrono, 0, 48000, 2, QByteArray());
    check(chrono.size() == TciClientCodec::kHeaderBytes,
          "a chrono frame is header-only");
    const auto chronoHeader = TciClientCodec::parseStreamHeader(chrono);
    check(chronoHeader && chronoHeader->length == 0, "a chrono frame declares zero samples");
}

}  // namespace

int main()
{
    testInitBurstParsing();
    testMalformedInputIsSurvivable();
    testModeMapping();
    testCommandFormatting();
    testStreamHeaderParsing();
    testFloat32Payload();
    testTruncatedPayloadIsClamped();
    testIntegerFormats();
    testOutboundFrameRoundTrip();

    if (g_failures != 0) {
        std::fprintf(stderr, "tci_client_codec_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("tci_client_codec_test: all checks passed\n");
    return 0;
}
