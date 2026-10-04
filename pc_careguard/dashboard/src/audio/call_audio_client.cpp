#include "audio/call_audio_client.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QIODevice>
#include <QMediaDevices>
#include <QMetaObject>
#include <QWebSocket>
#include <QtDebug>

namespace {
constexpr int kSampleRate = 16000;
constexpr int kChannels = 1;
constexpr int kFrameMs = 20;
constexpr int kFrameSamples = kSampleRate * kFrameMs / 1000;  // 320
constexpr int kFrameBytes = kFrameSamples * 2;                // S16LE, 채널당 2바이트 * mono
constexpr int kOpusMaxPacketBytes = 4000;                     // opus_encode 출력 버퍼 상한(권장값)
constexpr int kPlaybackBufferBytes = kSampleRate * 2 * 100 / 1000;  // ~100ms 분량

QAudioFormat makeFormat() {
    QAudioFormat format;
    format.setSampleRate(kSampleRate);
    format.setChannelCount(kChannels);
    format.setSampleFormat(QAudioFormat::Int16);
    return format;
}
}  // namespace

CallAudioClient::CallAudioClient(QObject* parent) : QObject(parent) {}

CallAudioClient::~CallAudioClient() { stop(); }

QString CallAudioClient::wsUrlFor(const QString& serverUrl, const QString& robotCode,
                                   const QString& direction) const {
    QString url = QString(serverUrl).replace(QStringLiteral("https://"), QStringLiteral("wss://"))
                      .replace(QStringLiteral("http://"), QStringLiteral("ws://"));
    return QStringLiteral("%1/ws/dashboard/%2/audio/%3").arg(url, robotCode, direction);
}

void CallAudioClient::start(const QString& serverUrl, const QString& robotCode) {
    if (active_) return;
    active_ = true;
    encodeErrorLogged_ = false;

    QString reason;
    if (!createCodecs(&reason) || !setupMicCapture(&reason) || !setupSpeakerPlayback(&reason)) {
        fail(reason);
        return;
    }
    openSockets(serverUrl, robotCode);
}

bool CallAudioClient::createCodecs(QString* reason) {
    int error = OPUS_OK;
    encoder_ = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_VOIP, &error);
    if (encoder_ == nullptr || error != OPUS_OK) {
        *reason = QStringLiteral("음성 인코더를 만들지 못했습니다 (%1)").arg(QString::fromUtf8(opus_strerror(error)));
        return false;
    }
    opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(10));

    decoder_ = opus_decoder_create(kSampleRate, kChannels, &error);
    if (decoder_ == nullptr || error != OPUS_OK) {
        *reason = QStringLiteral("음성 디코더를 만들지 못했습니다 (%1)").arg(QString::fromUtf8(opus_strerror(error)));
        return false;
    }
    return true;
}

void CallAudioClient::openSockets(const QString& serverUrl, const QString& robotCode) {
    // 연결 자체를 못 하는 경우(서버 미기동·네트워크 단절)엔 disconnected가 안 오고 오류 시그널만 온다.
    auto watch = [this](QWebSocket* socket, const QString& name) {
        connect(socket, &QWebSocket::connected, this, [socket] { socket->setProperty("everConnected", true); });
        connect(socket, &QWebSocket::disconnected, this, [this, socket] {
            // 연결에 한 번도 성공 못 한 채 끊긴 것은 "통화 중 끊김"이 아니라 연결 실패다 — 곧이어 오는
            // errorOccurred가 audioFailed로 알리므로, 여기서 audioDisconnected까지 보내면 같은 일이
            // 두 번 다른 의미로 보고된다.
            if (active_ && socket->property("everConnected").toBool()) emit audioDisconnected();
        });
        connect(socket, &QWebSocket::errorOccurred, this, [this, name](QAbstractSocket::SocketError) {
            if (active_) fail(QStringLiteral("%1 오디오 서버에 연결하지 못했습니다").arg(name));
        });
    };

    uplinkSocket_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(uplinkSocket_, &QWebSocket::binaryMessageReceived, this, &CallAudioClient::onUplinkBinaryMessage);
    watch(uplinkSocket_, QStringLiteral("수신"));
    uplinkSocket_->open(QUrl(wsUrlFor(serverUrl, robotCode, QStringLiteral("uplink"))));

    downlinkSocket_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    watch(downlinkSocket_, QStringLiteral("송신"));
    downlinkSocket_->open(QUrl(wsUrlFor(serverUrl, robotCode, QStringLiteral("downlink"))));
}

void CallAudioClient::fail(const QString& reason) {
    if (!active_) return;
    qWarning() << "통화 오디오 시작/연결 실패:" << reason;
    // teardown 중 소켓 close()가 같은 오류 시그널을 다시 일으키므로, 재진입하지 않게 먼저 내린다
    // (stop()과 같은 이유 — 안 그러면 한 번의 실패가 수십 번의 fail() 호출로 번진다).
    active_ = false;
    teardown();
    // 호출부가 start() 직후에 "통화 중" 표시를 하므로, 같은 호출 안에서 알리면 그 표시에 다시
    // 덮인다 — 이벤트 루프로 넘겨 그 뒤에 처리되게 한다.
    QMetaObject::invokeMethod(this, [this, reason] { emit audioFailed(reason); }, Qt::QueuedConnection);
}

bool CallAudioClient::setupMicCapture(QString* reason) {
    const QAudioDevice input = QMediaDevices::defaultAudioInput();
    if (input.isNull()) {
        *reason = QStringLiteral("사용할 수 있는 마이크가 없습니다");
        return false;
    }
    micSource_ = new QAudioSource(input, makeFormat(), this);
    micDevice_ = micSource_->start();
    if (micDevice_ == nullptr || micSource_->error() != QAudio::NoError) {
        *reason = QStringLiteral("마이크를 열지 못했습니다 (장치 사용 중이거나 권한이 없습니다)");
        return false;
    }
    connect(micDevice_, &QIODevice::readyRead, this, &CallAudioClient::onMicDataReady);
    return true;
}

bool CallAudioClient::setupSpeakerPlayback(QString* reason) {
    const QAudioDevice output = QMediaDevices::defaultAudioOutput();
    if (output.isNull()) {
        *reason = QStringLiteral("사용할 수 있는 스피커가 없습니다");
        return false;
    }
    speakerSink_ = new QAudioSink(output, makeFormat(), this);
    speakerSink_->setBufferSize(kPlaybackBufferBytes);  // ~100ms 지터 버퍼 역할
    speakerDevice_ = speakerSink_->start();
    if (speakerDevice_ == nullptr || speakerSink_->error() != QAudio::NoError) {
        *reason = QStringLiteral("스피커를 열지 못했습니다");
        return false;
    }
    return true;
}

void CallAudioClient::onMicDataReady() {
    if (micDevice_ == nullptr || encoder_ == nullptr) return;
    micPcmBuffer_.append(micDevice_->readAll());

    while (micPcmBuffer_.size() >= kFrameBytes) {
        const auto* pcm = reinterpret_cast<const opus_int16*>(micPcmBuffer_.constData());
        unsigned char encoded[kOpusMaxPacketBytes];
        const int encodedBytes = opus_encode(encoder_, pcm, kFrameSamples, encoded, kOpusMaxPacketBytes);
        micPcmBuffer_.remove(0, kFrameBytes);

        if (encodedBytes < 0) {
            // 이 프레임만 건너뛴다. 20ms마다 반복될 수 있어 첫 오류만 기록한다.
            if (!encodeErrorLogged_) {
                qWarning() << "Opus 인코딩 실패, 프레임을 건너뜁니다:" << opus_strerror(encodedBytes);
                encodeErrorLogged_ = true;
            }
            continue;
        }
        if (encodedBytes > 0 && downlinkSocket_ != nullptr &&
            downlinkSocket_->state() == QAbstractSocket::ConnectedState) {
            downlinkSocket_->sendBinaryMessage(
                QByteArray(reinterpret_cast<const char*>(encoded), encodedBytes));
        }
    }
}

void CallAudioClient::onUplinkBinaryMessage(const QByteArray& data) {
    if (decoder_ == nullptr || speakerDevice_ == nullptr) return;

    opus_int16 pcm[kFrameSamples * kChannels];
    const int decodedSamples = opus_decode(
        decoder_, reinterpret_cast<const unsigned char*>(data.constData()), data.size(), pcm, kFrameSamples, 0);
    if (decodedSamples <= 0) return;  // 손상된 프레임 — 이번 프레임만 건너뛴다

    speakerDevice_->write(reinterpret_cast<const char*>(pcm), decodedSamples * kChannels * 2);
}

void CallAudioClient::stop() {
    if (!active_) return;
    active_ = false;  // teardown 중 disconnected 시그널이 audioDisconnected를 다시 emit하지 않게 먼저 내린다
    teardown();
}

void CallAudioClient::teardown() {
    if (micSource_ != nullptr) {
        micSource_->stop();
        micSource_->deleteLater();
        micSource_ = nullptr;
    }
    micDevice_ = nullptr;

    if (speakerSink_ != nullptr) {
        speakerSink_->stop();
        speakerSink_->deleteLater();
        speakerSink_ = nullptr;
    }
    speakerDevice_ = nullptr;

    if (uplinkSocket_ != nullptr) {
        uplinkSocket_->close();
        uplinkSocket_->deleteLater();
        uplinkSocket_ = nullptr;
    }
    if (downlinkSocket_ != nullptr) {
        downlinkSocket_->close();
        downlinkSocket_->deleteLater();
        downlinkSocket_ = nullptr;
    }

    if (encoder_ != nullptr) {
        opus_encoder_destroy(encoder_);
        encoder_ = nullptr;
    }
    if (decoder_ != nullptr) {
        opus_decoder_destroy(decoder_);
        decoder_ = nullptr;
    }

    micPcmBuffer_.clear();
}
