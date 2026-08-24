#pragma once

#ifdef HAVE_WEBSOCKETS

#include "core/backends/IRadioBackend.h"
#include "core/backends/tci/TciClientCodec.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QString>
#include <QStringList>

#include <memory>

class QWebSocket;
class QTimer;

namespace AetherSDR {

class Resampler;

namespace tci {

// ── AetherSDR as a TCI *client* ────────────────────────────────────────────
//
// Every other TCI file in this tree is the server direction: AetherSDR
// impersonating a radio for a logging program. This is the reverse — a
// backend that DIALS a TCI server and treats whatever is behind it as the
// radio. It exists because that puts an entire class of hardware in reach
// that AetherSDR has no wire protocol for: the k3-tci-bridge project fronts
// an Elecraft K3/K3S with a Raspberry Pi, and anything else speaking TCI
// (ExpertSDR3, SunSDR) arrives for free.
//
// A pure seam backend, like Icom and HL2: it owns no RadioConnection and no
// PanadapterStream, so every model update leaves here as a normalized delta.
//
// NO SPECTRUM, and that is a property of the radio rather than a gap here.
// A K3 has no panadapter output to bridge — the bridge project measured the
// P3's tap and documented why it cannot supply one — so capabilities()
// reports maxPanadapters = 0 and the UI omits the surface rather than
// offering a window that would stay empty. The TCI IQ and (AetherSDR's own
// extension) spectrum frame types are handled the same way: if a server ever
// sends them we would need a panadapter to put them in, so v1 drops them.
//
// RADIO-AUTHORITATIVE. The server reports its own vfo/modulation/filter in
// the init burst and on every front-panel change, so this backend declares NO
// clientSettingsDomains and is never pushed a restored state (Constitution
// Principles II and III). What the radio says it is doing wins.
class TciBackend : public IRadioBackend {
    Q_OBJECT
    friend class TciBackendTest;

public:
    explicit TciBackend(QObject* parent = nullptr);
    ~TciBackend() override;

    // ---- identity & capability ----
    [[nodiscard]] RadioCapabilities capabilities() const override { return m_caps; }

    // TRUE, and load-bearing. RX audio arrives here as TCI binary frames and
    // leaves over audioFrameReady; there is no PanadapterStream carrying it.
    // Answering false would leave the operator with a connected radio and
    // silence (and answering it by family name is the #4490 shape).
    [[nodiscard]] bool ownsRxAudio() const override { return true; }

    // ---- connection lifecycle ----
    void connectRadio(const RadioConnectRequest& request) override;
    void disconnectRadio() override;
    [[nodiscard]] bool isConnected() const override { return m_connected; }

    // ---- intents DOWN ----
    void setSliceFrequency(int sliceId, double hz) override;
    void setSliceMode(int sliceId, const QString& mode) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) override;
    void setPanCenter(const QString& panId, double hz, PanCenterIntent intent) override;
    void setKeying(bool key) override;
    void invokeExtension(const QString& ns, const QString& verb,
                         quint64 requestId, const QVariant& arg = {}) override;

private slots:
    void onSocketConnected();
    void onSocketDisconnected();
    void onTextMessage(const QString& message);
    void onBinaryMessage(const QByteArray& frame);

private:
    void handleEvent(const TciEvent& event);
    void publishInitialSlice();
    void publishMeterDefs();
    void applyDeviceIdentity();
    void sendCommand(const QString& command);
    void resetSessionState();

    // This backend addresses exactly one slice and one (absent) panadapter,
    // for the same reason Icom does: one receiver, one transmitter. TCI's
    // `channels_count:2` is VFO A and VFO B on that ONE receiver — a split
    // pair, not a second demodulator — so it does not become a second slice.
    [[nodiscard]] static int sliceId() noexcept { return 0; }
    [[nodiscard]] static QString panId() { return QStringLiteral("0"); }

    QWebSocket* m_socket = nullptr;
    QTimer*     m_reconnectTimer = nullptr;

    RadioCapabilities m_caps;
    bool m_connected = false;      // TCI `ready;` seen, not merely socket-open
    bool m_socketOpen = false;
    bool m_slicePublished = false;

    // The trx this backend drives. TCI numbers transceivers from 0 and the
    // bridge reports trx_count:1, so 0 is the only one that exists today.
    int m_trx = 0;

    // Last state the SERVER reported, so a delta can be rebuilt without
    // asking. TCI notifications are per-field, and SliceModel wants the
    // frequency in MHz while the wire carries Hz.
    qint64  m_vfoHz = 0;
    QString m_mode;
    int     m_filterLowHz = 0;
    int     m_filterHighHz = 0;
    bool    m_transmitting = false;

    // Audio: the wire rate the server advertised, and the converter that takes
    // it to the engine's native 24 kHz stereo float. Built on the FIRST audio
    // frame rather than at connect, because the frame header is the only place
    // the true rate and channel count are stated for the stream we actually
    // get — `audio_samplerate` in the init burst is what the server intends.
    int m_audioSampleRate = 0;
    int m_audioChannels = 0;
    std::unique_ptr<Resampler> m_resampler;

    // Deferred so a malformed or unsupported stream is reported once rather
    // than once per frame at 23 frames a second.
    bool m_audioFormatWarned = false;
};

}  // namespace tci
}  // namespace AetherSDR

#endif  // HAVE_WEBSOCKETS
