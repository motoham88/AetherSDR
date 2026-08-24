#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace AetherSDR::tci {

// ── The CLIENT half of TCI, and why it is not src/core/TciProtocol ─────────
//
// src/core/TciProtocol is the SERVER direction: it receives commands from a
// logging program, mutates RadioModel, and answers. This is the inverse — it
// receives a radio's unsolicited notifications and turns them into values a
// backend can emit as deltas, and it formats the commands we send UP to that
// radio. Same wire vocabulary, opposite data flow, so the two tables are not
// interchangeable and must not be merged.
//
// The mode tables prove the point. The server maps AetherSDR's CW to the wire
// as `cw` and CWL as `cwr`; the K3 bridge that motivated this backend
// advertises `cwl,cwu` and never emits either of those spellings. A client
// that reused the server's table would read every CW contact on the wrong
// sideband and report no error (k3-tci-command-map.md, "Sideband polarity").
// So this table is alias-tolerant in the inbound direction on purpose.
//
// NO I/O and no Qt object here: everything is a static pure function over
// strings and bytes, so the whole protocol surface is unit-testable without a
// socket, a radio, or an event loop.

// One parsed TCI notification: a verb and its comma-separated arguments,
// already stripped of the trailing `;`.
struct TciEvent {
    QString     verb;
    QStringList args;

    // Convenience accessors — TCI is positional and every argument is
    // optional in practice, so a missing or unparseable field must yield the
    // caller's fallback rather than a zero that reads as a real value.
    [[nodiscard]] int     argInt(int index, int fallback = 0) const;
    [[nodiscard]] qint64  argLongLong(int index, qint64 fallback = 0) const;
    [[nodiscard]] bool    argBool(int index, bool fallback = false) const;
    [[nodiscard]] QString argString(int index, const QString& fallback = {}) const;
};

// The TCI binary stream header, decode direction. Byte-identical to the
// TciAudioHeader that TciServer.cpp writes — 16 x uint32, little-endian.
// Kept as a separate declaration rather than shared with the server's because
// that one is a file-local struct in a .cpp; the static_assert below and the
// server's own are what keep the two honest.
struct TciStreamHeader {
    quint32 receiver   = 0;
    quint32 sampleRate = 0;
    quint32 format     = 0;   // 0=int16, 1=int24, 2=int32, 3=float32
    quint32 codec      = 0;
    quint32 crc        = 0;
    quint32 length     = 0;   // real samples in the payload
    quint32 type       = 0;   // 0=IQ, 1=RX_AUDIO, 2=TX_AUDIO, 3=TX_CHRONO
    quint32 channels   = 0;
    quint32 reserved[8] = {0, 0, 0, 0, 0, 0, 0, 0};
};
static_assert(sizeof(TciStreamHeader) == 64, "TCI stream header must be 64 bytes");

// Stream `type` values, named so call sites stop repeating magic numbers.
enum StreamType : quint32 {
    StreamIq       = 0,
    StreamRxAudio  = 1,
    StreamTxAudio  = 2,
    StreamTxChrono = 3,
};

// Sample `format` values.
enum SampleFormat : quint32 {
    FormatInt16   = 0,
    FormatInt24   = 1,
    FormatInt32   = 2,
    FormatFloat32 = 3,
};

class TciClientCodec {
public:
    // ── inbound: text ──────────────────────────────────────────────────────
    //
    // A TCI text message may carry SEVERAL `;`-terminated notifications in one
    // WebSocket frame — the init burst arrives that way — so parsing is
    // defined over a whole message, not a line. Empty and malformed segments
    // are dropped rather than surfaced: a client that disconnects on an
    // unknown verb cannot survive a server that grows one.
    [[nodiscard]] static QVector<TciEvent> parseMessage(const QString& message);

    // ── modes ──────────────────────────────────────────────────────────────
    //
    // Inbound is alias-tolerant (see the header comment); outbound emits the
    // spelling the widest set of servers accepts. `ok` reports whether the
    // wire mode was recognised at all, so a backend can decline to invent a
    // mode rather than silently landing on USB.
    [[nodiscard]] static QString modeFromWire(const QString& tciMode, bool* ok = nullptr);
    [[nodiscard]] static QString modeToWire(const QString& aetherMode);

    // ── outbound: commands ─────────────────────────────────────────────────
    // Each returns one complete `verb:args;` string ready to send.
    [[nodiscard]] static QString vfoSet(int trx, int channel, qint64 hz);
    [[nodiscard]] static QString modulationSet(int trx, const QString& aetherMode);
    [[nodiscard]] static QString rxFilterBandSet(int trx, int lowHz, int highHz);
    [[nodiscard]] static QString trxSet(int trx, bool transmitting);
    [[nodiscard]] static QString splitEnableSet(int trx, bool on);
    [[nodiscard]] static QString ritEnableSet(int trx, bool on);
    [[nodiscard]] static QString ritOffsetSet(int trx, int hz);
    [[nodiscard]] static QString xitEnableSet(int trx, bool on);
    [[nodiscard]] static QString driveSet(int trx, int percent);
    [[nodiscard]] static QString volumeSet(int db);
    [[nodiscard]] static QString muteSet(bool on);
    [[nodiscard]] static QString audioStart(int trx);
    [[nodiscard]] static QString audioStop(int trx);

    // ── inbound: binary ────────────────────────────────────────────────────
    //
    // Decodes one binary WebSocket frame. Returns the header when the frame is
    // well-formed and long enough to contain what it claims, nothing when it
    // is not — a short or corrupt frame is dropped, never partially consumed.
    [[nodiscard]] static std::optional<TciStreamHeader> parseStreamHeader(const QByteArray& frame);

    // Converts a stream frame's payload to the interleaved 32-bit float PCM
    // the audio seam carries, handling every `format` TCI defines. Returns
    // empty for an unsupported format rather than reinterpreting the bytes,
    // because guessing wrong here is full-scale noise into the operator's ears.
    [[nodiscard]] static QByteArray payloadToFloat32(const TciStreamHeader& header,
                                                     const QByteArray& frame);

    // Builds an outbound binary frame (TX audio / TX chrono) from float PCM.
    [[nodiscard]] static QByteArray buildStreamFrame(quint32 type, int trx, int sampleRate,
                                                     int channels, const QByteArray& float32Pcm);

    // Byte offset of the payload within a binary frame.
    static constexpr int kHeaderBytes = 64;
};

}  // namespace AetherSDR::tci
