#include "core/backends/tci/TciBackend.h"

#ifdef HAVE_WEBSOCKETS

#include <QLoggingCategory>
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
    if (!m_socket) return;

    // STOP THE STREAM FIRST, on this path too. disconnectRadio() does it, but
    // app teardown does not go through disconnectRadio() — it destroys the
    // backend — so without this a quit while receiving leaves the server
    // encoding audio for a client that no longer exists until its own timeout
    // notices. Cheap to send and harmless if the socket is already going away.
    if (m_socketOpen) sendCommand(TciClientCodec::audioStop(m_trx));

    // Then close rather than abort, so the server sees a clean WebSocket close
    // instead of a dropped TCP connection it has to time out separately.
    m_socket->close();
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
    qCInfo(lcTci) << "TCI server closed the connection";

    // CLEAR EVERYTHING, not just the two connection flags. A server-side drop
    // — the Pi rebooting, systemd restarting the bridge — has to leave exactly
    // the state a fresh connect would find, and the flag that matters most is
    // m_slicePublished: left true, publishInitialSlice() no-ops on the next
    // session and the slice never comes back. The stale Resampler is the same
    // shape of problem, still holding filter state for a stream that ended.
    //
    // This path is reachable ONLY from the server side; a client-initiated
    // disconnect goes through disconnectRadio(), which resets separately. That
    // asymmetry is exactly why this was missed.
    resetSessionState();
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
            qCInfo(lcTci) << "TCI session ready:" << m_caps.model
                          << "canTransmit=" << m_caps.canTransmit;
            emit connected();

            // BEFORE the first value can arrive. A meter VALUE whose meter
            // has no definition is silently discarded — MeterModel::
            // updateValueByName looks the id up and returns false when it
            // finds nothing — so without this every rx_smeter reading is
            // computed, emitted, and dropped on the floor. That is the
            // orphaned-meter-seam defect IcomCivBackend and SimBackend both
            // carry warnings about, and it is invisible from here: the
            // backend goes on emitting correct values forever.
            publishMeterDefs();

            // AFTER connected(), never before — the ordering IcomCivBackend
            // uses, and it is load-bearing rather than stylistic. RadioModel
            // reacts to connected() by staging and clearing the previous
            // session's slice and pan models, so a slice published first is
            // created and then immediately swept away with them. Measured
            // against a live K3: the session came up with the radio reporting
            // ready, audio streaming, and sliceCount 0.
            publishInitialSlice();

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
        if (!m_connected) return;   // cached; published by publishInitialSlice

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
        if (!m_connected) return;   // cached; published by publishInitialSlice

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
        if (!m_connected) return;   // cached; published by publishInitialSlice

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
        || verb == QLatin1String("rit_offset") || verb == QLatin1String("xit_offset")) {
        if (event.argInt(0, -1) != m_trx) return;
        SliceDelta d;
        if (verb == QLatin1String("rit_enable")) {
            m_ritOn = event.argBool(1, false);
            d.ritOn = m_ritOn;
        }
        if (verb == QLatin1String("xit_enable")) {
            m_xitOn = event.argBool(1, false);
            d.xitOn = m_xitOn;
        }
        // ONE cached offset for both, because the K3 has one RO register and
        // the server echoes the same value on both verbs.
        if (verb == QLatin1String("rit_offset")) {
            m_ritOffsetHz = event.argInt(1, 0);
            d.ritFreq = m_ritOffsetHz;
        }
        // `xit_offset` was previously unhandled, so a server that reports the
        // transmit offset separately left the XIT readout pinned at 0 while
        // the radio was shifted.
        if (verb == QLatin1String("xit_offset")) {
            m_ritOffsetHz = event.argInt(1, 0);
            d.xitFreq = m_ritOffsetHz;
        }
        if (!m_connected) return;   // cached; published by publishInitialSlice
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

void TciBackend::publishMeterDefs()
{
    // ONE meter. TCI's only receive telemetry is `rx_smeter`, and the source/
    // name/unit triple matches what IcomMeters and Hl2Backend already publish
    // for the same reading, so the existing S-meter consumers find it without
    // knowing which family produced it.
    MeterDef sMeter;
    sMeter.index  = 0;
    sMeter.source = QStringLiteral("SLC");
    sMeter.name   = QStringLiteral("LEVEL");
    sMeter.unit   = QStringLiteral("dBm");
    sMeter.low    = -140.0;
    sMeter.high   = -10.0;
    // RELATIVE, and said out loud. TCI carries no calibration and the bridge
    // this was developed against records two failed attempts to establish one
    // against the radio's own attenuator — both of which were measuring
    // propagation rather than the step. An operator reading these as absolute
    // dBm would be reading a number nobody has justified.
    sMeter.description = QStringLiteral("Receive signal level (uncalibrated — relative)");
    emit meterDefined(sMeter);
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

    // SEEDED FROM THE INIT BURST, and the ONLY place any of it is published.
    //
    // NOTHING may emit a slice delta before connected(): RadioModel
    // materialises a SliceModel on the first delta it sees, and then stages
    // and clears the models when connected() arrives. A delta emitted early
    // therefore builds a slice the GUI binds to and the session then discards
    // — leaving the applet showing a frozen snapshot of the init burst while
    // every later delta lands on a different instance. Measured: the model
    // read 7.0255 MHz with RIT at 1234 Hz while the RX applet showed 7.032180
    // and +750 Hz, and no amount of tuning moved it.
    //
    // So the per-field handlers cache and return while !m_connected, and this
    // publishes the whole accumulated state once, after connected(). The burst reports vfo/modulation/
    // rx_filter_band BEFORE the `ready;` that lets this slice exist, so the
    // per-field handlers emitted them into a slice that was not there yet and
    // every one was dropped. Measured against a live K3 sitting on 14.028970
    // CWL with a 400 Hz filter: the slice came up at 0 Hz in USB, and stayed
    // there until the operator happened to touch something on the radio.
    if (m_vfoHz > 0) s.frequency = static_cast<double>(m_vfoHz) / 1.0e6;   // MHz
    if (!m_mode.isEmpty()) s.mode = m_mode;
    if (m_filterHighHz > m_filterLowHz) {
        s.filterLow  = m_filterLowHz;
        s.filterHigh = m_filterHighHz;
    }
    s.ritOn   = m_ritOn;
    s.xitOn   = m_xitOn;
    s.ritFreq = m_ritOffsetHz;
    s.xitFreq = m_ritOffsetHz;
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

void TciBackend::setRitEnabled(bool on)
{
    sendCommand(TciClientCodec::ritEnableSet(m_trx, on));
}

void TciBackend::setXitEnabled(bool on)
{
    sendCommand(TciClientCodec::xitEnableSet(m_trx, on));
}

void TciBackend::setRitOffset(int hz)
{
    // Fire-and-forget with no optimistic local update, like every other verb
    // here: the server answers with the offset the radio ACCEPTED, which is
    // not always the one asked for. On a K3 the register is shared with XIT
    // and clamped to +/-9999, so the echo is the only honest source.
    sendCommand(TciClientCodec::ritOffsetSet(m_trx, hz));
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
