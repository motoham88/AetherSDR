#include "core/backends/tci/TciBackend.h"

#ifdef HAVE_WEBSOCKETS

#include <QLoggingCategory>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#include <cmath>

#include "core/Resampler.h"

namespace AetherSDR::tci {

Q_LOGGING_CATEGORY(lcTci, "aether.tci.client")
Q_LOGGING_CATEGORY(lcTciAudio, "aether.tci.client.audio")

namespace {

// The engine's native RX-audio format, named once here so the conversion
// below reads as a conversion TO something rather than a magic 24000.
// RadioModel::rxDemodAudioReady spells the contract out in its parameter name:
// pcm24kStereoFloat.
constexpr double kEngineRateHz = 24000.0;
constexpr int    kEngineChannels = 2;

// The TCI default. Only used when a server connects without stating a rate.
constexpr int kDefaultTciRateHz = 48000;

// r8brain needs a ceiling on the block it will be handed. The bridge sends
// 2048-sample frames; this leaves room for a server that sends larger ones
// without reallocating per frame.
constexpr int kMaxResampleBlock = 8192;

}  // namespace

TciBackend::TciBackend(QObject* parent)
    : IRadioBackend(parent)
{
    // Declared before any wire traffic so a consumer that reads capabilities()
    // between construction and connect gets the truth about this family rather
    // than the struct's all-false defaults.
    m_caps.family = QStringLiteral("tci");
    m_caps.manufacturer.clear();
    m_caps.model.clear();

    // ONE slice. See the header: TCI channels are VFO A/B, not demodulators.
    m_caps.maxSlices = 1;

    // ZERO panadapters — the load-bearing default from RadioCapabilities.h.
    // A K3 has no spectrum to bridge, and the UI reads this to omit the
    // panadapter surface entirely rather than opening an empty window.
    m_caps.maxPanadapters = 0;

    // Corrected from `receive_only` in the init burst. Starting false means a
    // server that never states it cannot key by accident — the safe direction
    // for a capability the TX guard consults.
    m_caps.canTransmit = false;

    m_caps.sampleRatesHz = {kDefaultTciRateHz};
}

TciBackend::~TciBackend()
{
    // The socket is a child QObject and would be destroyed anyway; closing
    // first means the server sees a clean close rather than a dropped TCP
    // connection it has to time out.
    if (m_socket) m_socket->abort();
}

// ── connection lifecycle ───────────────────────────────────────────────────

void TciBackend::connectRadio(const RadioConnectRequest& request)
{
    disconnectRadio();
    resetSessionState();

    const quint16 port = request.port != 0 ? request.port : quint16(50001);
    const QUrl url(QStringLiteral("ws://%1:%2").arg(request.host).arg(port));

    m_socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(m_socket, &QWebSocket::connected,          this, &TciBackend::onSocketConnected);
    connect(m_socket, &QWebSocket::disconnected,       this, &TciBackend::onSocketDisconnected);
    connect(m_socket, &QWebSocket::textMessageReceived,   this, &TciBackend::onTextMessage);
    connect(m_socket, &QWebSocket::binaryMessageReceived, this, &TciBackend::onBinaryMessage);

    connect(m_socket, &QWebSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) {
        const QString reason = m_socket ? m_socket->errorString()
                                        : QStringLiteral("unknown socket error");
        qCWarning(lcTci) << "TCI connect failed:" << reason;
        emit connectionError(reason);
    });

    qCInfo(lcTci) << "dialing TCI server" << url.toString();
    m_socket->open(url);
}

void TciBackend::disconnectRadio()
{
    if (!m_socket) return;

    // Stop the audio stream before dropping the link. A server that keeps a
    // per-client stream armed will otherwise go on encoding for a client that
    // is gone until its own timeout notices.
    if (m_socketOpen) sendCommand(TciClientCodec::audioStop(m_trx));

    m_socket->close();
    m_socket->deleteLater();
    m_socket = nullptr;

    const bool wasConnected = m_connected;
    resetSessionState();
    if (wasConnected) emit disconnected();
}

void TciBackend::resetSessionState()
{
    m_connected = false;
    m_socketOpen = false;
    m_slicePublished = false;
    m_vfoHz = 0;
    m_mode.clear();
    m_filterLowHz = 0;
    m_filterHighHz = 0;
    m_transmitting = false;
    m_audioSampleRate = 0;
    m_audioChannels = 0;
    m_audioFormatWarned = false;
    m_resampler.reset();
}

void TciBackend::onSocketConnected()
{
    m_socketOpen = true;
    qCInfo(lcTci) << "WebSocket open — awaiting TCI init burst";

    // DELIBERATELY SILENT HERE. TCI is server-speaks-first: the init burst
    // arrives unprompted and ends with `ready;`. Sending commands before it
    // lands would be asking questions the server is already answering, and a
    // request against a not-yet-declared trx count is unanswerable.
}

void TciBackend::onSocketDisconnected()
{
    const bool wasConnected = m_connected;
    m_socketOpen = false;
    m_connected = false;
    qCInfo(lcTci) << "TCI server closed the connection";
    if (wasConnected) emit disconnected();
}

// ── inbound text ───────────────────────────────────────────────────────────

void TciBackend::onTextMessage(const QString& message)
{
    const QVector<TciEvent> events = TciClientCodec::parseMessage(message);
    for (const TciEvent& event : events) handleEvent(event);
}

void TciBackend::applyDeviceIdentity()
{
    // The status bar shows manufacturer ABOVE model, and only when the model
    // does not already carry it (RadioCapabilities::manufacturer). TCI gives
    // us one free-form `device` string — "Elecraft K3" — which already names
    // the maker, so claiming a separate manufacturer would render as a
    // stutter. Left empty on purpose.
    m_caps.manufacturer.clear();
}

void TciBackend::handleEvent(const TciEvent& event)
{
    const QString& verb = event.verb;

    // ── init burst: capability declarations ────────────────────────────────
    if (verb == QLatin1String("device")) {
        m_caps.model = event.argString(0);
        applyDeviceIdentity();
        emit capabilitiesChanged();
        return;
    }

    if (verb == QLatin1String("receive_only")) {
        // The wire says what it CANNOT do; capabilities says what it can.
        m_caps.canTransmit = !event.argBool(0, false);
        emit capabilitiesChanged();
        return;
    }

    if (verb == QLatin1String("trx_count")) {
        // Recorded but NOT turned into maxSlices. A second trx is a second
        // receiver, and wiring one is more than a capability bump — it needs
        // its own slice identity and audio routing. v1 drives trx 0.
        const int count = event.argInt(0, 1);
        if (count > 1) {
            qCInfo(lcTci) << "server reports" << count
                          << "transceivers; this backend drives trx 0 only";
        }
        return;
    }

    if (verb == QLatin1String("vfo_limits")) {
        // Both endpoints or neither: a half-parsed range would tell the band
        // buttons a lie, and RadioCapabilities documents 0/0 as "not reported",
        // which leaves the caller's own assumption intact.
        if (event.args.size() >= 2) {
            const qint64 low  = event.argLongLong(0, 0);
            const qint64 high = event.argLongLong(1, 0);
            if (high > low) {
                m_caps.tuningMinHz = static_cast<double>(low);
                m_caps.tuningMaxHz = static_cast<double>(high);
                emit capabilitiesChanged();
            }
        }
        return;
    }

    if (verb == QLatin1String("modulations_list")) {
        // What the RADIO will demodulate, in AetherSDR's vocabulary. Unknown
        // wire spellings are dropped rather than guessed — offering a mode the
        // server will reject is worse than not offering it.
        QStringList modes;
        for (const QString& raw : event.args) {
            bool ok = false;
            const QString mode = TciClientCodec::modeFromWire(raw, &ok);
            if (ok && !modes.contains(mode)) modes.append(mode);
        }
        if (!modes.isEmpty()) {
            SliceDelta d;
            d.modeList = modes;
            emit sliceChanged(sliceId(), d);
        }
        return;
    }

    if (verb == QLatin1String("audio_samplerate")) {
        const int rate = event.argInt(0, kDefaultTciRateHz);
        if (rate > 0) {
            m_caps.sampleRatesHz = {rate};
            emit capabilitiesChanged();
        }
        return;
    }

    if (verb == QLatin1String("ready")) {
        // `ready;` — not socket-open — is when this radio becomes usable: the
        // capability declarations are all in, so a consumer reacting to
        // connected() reads a fully-formed capabilities() rather than the
        // half-populated struct it would see a few milliseconds earlier.
        if (!m_connected) {
            m_connected = true;
            publishInitialSlice();
            qCInfo(lcTci) << "TCI session ready:" << m_caps.model
                          << "canTransmit=" << m_caps.canTransmit;
            emit connected();

            // Ask for RX audio only once the session is real. The bridge keys
            // its per-client stream off this and sends nothing until it
            // arrives — a client that never asks hears silence and sees no
            // error at either end.
            sendCommand(TciClientCodec::audioStart(m_trx));
        }
        return;
    }

    // ── running state ──────────────────────────────────────────────────────
    if (verb == QLatin1String("vfo")) {
        // vfo:<trx>,<channel>,<hz>. Channel 1 is VFO B — split's other half —
        // which this backend tracks but does not publish as a slice frequency,
        // because SliceModel has exactly one frequency per slice and it is the
        // one being received.
        if (event.argInt(0, -1) != m_trx) return;
        if (event.argInt(1, -1) != 0) return;

        const qint64 hz = event.argLongLong(2, 0);
        if (hz <= 0 || hz == m_vfoHz) return;
        m_vfoHz = hz;

        SliceDelta d;
        d.frequency = static_cast<double>(hz) / 1.0e6;   // MHz, per SliceDelta
        emit sliceChanged(sliceId(), d);
        return;
    }

    if (verb == QLatin1String("modulation")) {
        if (event.argInt(0, -1) != m_trx) return;
        bool ok = false;
        const QString mode = TciClientCodec::modeFromWire(event.argString(1), &ok);
        if (!ok) {
            qCWarning(lcTci) << "server reported an unmapped modulation:"
                             << event.argString(1);
            return;
        }
        if (mode == m_mode) return;
        m_mode = mode;

        SliceDelta d;
        d.mode = mode;
        emit sliceChanged(sliceId(), d);
        return;
    }

    if (verb == QLatin1String("rx_filter_band")) {
        if (event.argInt(0, -1) != m_trx) return;
        const int low  = event.argInt(1, 0);
        const int high = event.argInt(2, 0);
        if (high <= low) return;
        if (low == m_filterLowHz && high == m_filterHighHz) return;
        m_filterLowHz = low;
        m_filterHighHz = high;

        SliceDelta d;
        d.filterLow = low;
        d.filterHigh = high;
        emit sliceChanged(sliceId(), d);
        return;
    }

    if (verb == QLatin1String("rx_smeter")) {
        // "SLC:LEVEL" — the SOURCE:NAME form every meter consumer looks up by.
        // Emitting the bare name publishes a meter nothing can find, which is
        // the orphaned-meter-seam defect recorded in IcomCivBackend.
        if (event.argInt(0, -1) != m_trx) return;
        emit meterUpdate(QStringLiteral("SLC:LEVEL"),
                         static_cast<double>(event.argInt(1, -140)));
        return;
    }

    if (verb == QLatin1String("trx")) {
        if (event.argInt(0, -1) != m_trx) return;
        const bool tx = event.argBool(1, false);
        if (tx == m_transmitting) return;
        m_transmitting = tx;

        TransmitDelta d;
        d.mox = tx;
        emit transmitChanged(d);
        return;
    }

    if (verb == QLatin1String("drive")) {
        if (event.argInt(0, -1) != m_trx) return;
        TransmitDelta d;
        d.rfPower = qBound(0, event.argInt(1, 0), 100);
        emit transmitChanged(d);
        return;
    }

    if (verb == QLatin1String("rit_enable") || verb == QLatin1String("xit_enable")
        || verb == QLatin1String("rit_offset")) {
        if (event.argInt(0, -1) != m_trx) return;
        SliceDelta d;
        if (verb == QLatin1String("rit_enable"))  d.ritOn = event.argBool(1, false);
        if (verb == QLatin1String("xit_enable"))  d.xitOn = event.argBool(1, false);
        if (verb == QLatin1String("rit_offset"))  d.ritFreq = event.argInt(1, 0);
        emit sliceChanged(sliceId(), d);
        return;
    }

    // Everything else — start, protocol, split_enable, mute, mon_volume,
    // tx_profiles, spots — is either not yet mapped or has no consumer above
    // the seam. `split_enable` is the notable one: the seam carries no split
    // verb, so honouring it needs a neutral field before a decode. Ignored on
    // purpose: a client that fails on an unknown verb cannot survive a server
    // that grows one.
}

void TciBackend::publishInitialSlice()
{
    if (m_slicePublished) return;
    m_slicePublished = true;

    // ONE slice, existing from the moment the session is ready. Without it
    // nothing downstream has anything to attach audio to — the same reason
    // IcomCivBackend publishes its slice at connect.
    SliceDelta s;
    s.panId  = panId();
    s.inUse  = true;
    s.active = true;
    // One receiver IS the transmitter here. Leaving this unset makes
    // RadioModel's interlock refuse every key attempt with "No transmit slice
    // is assigned" before the backend is ever asked — a refusal that is
    // invisible from down here.
    s.txSlice = true;
    emit sliceChanged(sliceId(), s);
}

// ── inbound binary (RX audio) ──────────────────────────────────────────────

void TciBackend::onBinaryMessage(const QByteArray& frame)
{
    const auto header = TciClientCodec::parseStreamHeader(frame);
    if (!header) return;

    // Only RX audio for this trx. IQ (type 0) and the spectrum extension
    // (type 4) are dropped: with maxPanadapters = 0 there is nowhere to put
    // them, and a TX_CHRONO request means nothing until TX audio exists.
    if (header->type != StreamRxAudio) return;
    if (static_cast<int>(header->receiver) != m_trx) return;

    const QByteArray pcm = TciClientCodec::payloadToFloat32(*header, frame);
    if (pcm.isEmpty()) {
        if (!m_audioFormatWarned) {
            m_audioFormatWarned = true;
            qCWarning(lcTciAudio) << "dropping RX audio: unsupported format"
                                  << header->format << "codec" << header->codec;
        }
        return;
    }

    const int rate     = static_cast<int>(header->sampleRate);
    const int channels = qMax(1u, header->channels);
    if (rate <= 0) return;

    // Rebuild the converter when the stream's shape changes. The header is
    // the authority, not `audio_samplerate` from the init burst: that states
    // what the server intends, this is what actually arrived.
    if (rate != m_audioSampleRate || channels != m_audioChannels) {
        m_audioSampleRate = rate;
        m_audioChannels = channels;
        m_resampler.reset();
        if (std::fabs(static_cast<double>(rate) - kEngineRateHz) > 0.5) {
            m_resampler = std::make_unique<Resampler>(
                static_cast<double>(rate), kEngineRateHz, kMaxResampleBlock);
        }
        qCInfo(lcTciAudio) << "RX audio stream:" << rate << "Hz" << channels
                           << "ch →" << kEngineRateHz << "Hz stereo float"
                           << (m_resampler ? "(resampling)" : "(rate matches)");
    }

    const auto* samples = reinterpret_cast<const float*>(pcm.constData());
    const int sampleCount = pcm.size() / 4;
    if (sampleCount <= 0) return;

    QByteArray out;

    if (channels >= kEngineChannels) {
        // Interleaved stereo in. A partial trailing frame would swap the
        // channels of everything after it, so drop it rather than carry it.
        const int stereoFrames = sampleCount / 2;
        if (stereoFrames <= 0) return;
        out = m_resampler
            ? m_resampler->processStereoToStereo(samples, stereoFrames)
            : pcm;
    } else {
        // Mono in — duplicate to the stereo bus the engine carries.
        if (m_resampler) {
            out = m_resampler->processMonoToStereo(samples, sampleCount);
        } else {
            out.resize(sampleCount * 2 * 4);
            auto* dst = reinterpret_cast<float*>(out.data());
            for (int i = 0; i < sampleCount; ++i) {
                dst[i * 2]     = samples[i];
                dst[i * 2 + 1] = samples[i];
            }
        }
    }

    // r8brain has a startup interval that produces no output; an empty return
    // is that, not an error, and must not be forwarded as a zero-length frame.
    if (out.isEmpty()) return;

    emit audioFrameReady(out);
}

// ── intents DOWN ───────────────────────────────────────────────────────────

void TciBackend::sendCommand(const QString& command)
{
    if (!m_socket || !m_socketOpen) return;
    m_socket->sendTextMessage(command);
}

void TciBackend::setSliceFrequency(int sliceId, double hz)
{
    if (sliceId != TciBackend::sliceId() || hz <= 0.0) return;

    // FIRE AND FORGET, and no optimistic local update. The server answers
    // every accepted `vfo:` with its own `vfo:` notification carrying the
    // frequency it actually tuned — which is not always the one asked for,
    // since a TCI server fronting real hardware clamps to what the radio can
    // do. Publishing the request here and the answer a moment later would
    // make the display jump; publishing only the answer keeps it honest.
    sendCommand(TciClientCodec::vfoSet(m_trx, 0, static_cast<qint64>(std::llround(hz))));
}

void TciBackend::setSliceMode(int sliceId, const QString& mode)
{
    if (sliceId != TciBackend::sliceId() || mode.isEmpty()) return;
    sendCommand(TciClientCodec::modulationSet(m_trx, mode));
}

void TciBackend::setSliceFilter(int sliceId, int lowHz, int highHz)
{
    if (sliceId != TciBackend::sliceId() || highHz <= lowHz) return;
    sendCommand(TciClientCodec::rxFilterBandSet(m_trx, lowHz, highHz));
}

void TciBackend::setSliceAgc(int sliceId, const QString& mode, int thresholdDb)
{
    Q_UNUSED(sliceId);
    Q_UNUSED(mode);
    Q_UNUSED(thresholdDb);

    // NOT MAPPED, and silently doing nothing is the right no-op here rather
    // than an approximation. TCI carries no AGC verb at all, so there is
    // nothing to translate to; the bridge that motivated this backend
    // documents `agc_mode:off` as unreachable on the radio side too.
}

void TciBackend::setPanCenter(const QString& panId, double hz, PanCenterIntent intent)
{
    Q_UNUSED(panId);
    Q_UNUSED(hz);
    Q_UNUSED(intent);

    // No panadapter exists to centre — capabilities() reports zero of them, so
    // this cannot be reached from a UI that respects the capability, and a
    // caller that ignores it must not be able to move the VFO by dragging a
    // window that is not there.
}

void TciBackend::setKeying(bool key)
{
    // The decision to ALLOW keying is made above this seam by the engine
    // guard; this is the translation only. The capability check is repeated
    // here because it costs nothing and the alternative — a receive-only
    // server being told to transmit — has consequences the guard's absence
    // would not undo.
    if (!m_caps.canTransmit) return;
    sendCommand(TciClientCodec::trxSet(m_trx, key));
}

void TciBackend::invokeExtension(const QString& ns, const QString& verb,
                                 quint64 requestId, const QVariant& arg)
{
    Q_UNUSED(arg);

    // No extension namespace is defined for this family yet. Answering with
    // extensionError rather than staying silent matters: the caller minted a
    // requestId and is correlating a reply, so silence would leave it waiting
    // for one that is never coming.
    if (requestId != 0) {
        emit extensionError(requestId,
                            QStringLiteral("TCI backend has no extension '%1:%2'")
                                .arg(ns, verb));
    }
}

}  // namespace AetherSDR::tci

#endif  // HAVE_WEBSOCKETS
