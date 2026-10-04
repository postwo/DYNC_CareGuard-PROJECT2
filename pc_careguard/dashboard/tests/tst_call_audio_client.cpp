#include <QtTest>

#include "audio/call_audio_client.h"

// 통화 오디오는 시작하지 못하거나 서버에 연결하지 못했을 때 소리 없는 "통화 중"으로 남지 않고 실패를
// 알려야 한다. 오디오 장치가 없는 환경에서는 장치 단계에서, 있는 환경에서는 (닿지 않는 주소로) 소켓
// 연결 단계에서 실패한다 — 어느 쪽이든 같은 신호가 와야 한다.
class CallAudioClientTest : public QObject {
    Q_OBJECT

private slots:
    void startFailureIsReportedNotSilentlyIgnored() {
        CallAudioClient client;
        QSignalSpy failed(&client, &CallAudioClient::audioFailed);
        QSignalSpy disconnected(&client, &CallAudioClient::audioDisconnected);

        client.start("http://127.0.0.1:9", "R-01");  // 9번 포트(discard)는 아무도 안 듣는다

        QVERIFY2(failed.wait(5000), "실패가 5초 안에 보고되지 않았다");
        QCOMPARE(failed.count(), 1);  // 두 소켓이 모두 실패해도 한 번만 알린다
        QVERIFY(!failed.first().first().toString().isEmpty());
        QVERIFY(!client.isActive());
        QCOMPARE(disconnected.count(), 0);  // 실패는 audioFailed로만 알린다
    }

    void failureIsNotReportedInsideStartItself() {
        // 호출부가 start() 직후 "통화 중" 표시를 하므로, 같은 호출 안에서 알리면 그 표시에 덮인다.
        CallAudioClient client;
        QSignalSpy failed(&client, &CallAudioClient::audioFailed);
        client.start("http://127.0.0.1:9", "R-01");
        QCOMPARE(failed.count(), 0);
        QVERIFY(failed.wait(5000));
    }

    void stopAfterFailureIsSafe() {
        CallAudioClient client;
        client.stop();  // 시작한 적 없어도 안전해야 한다
        QVERIFY(!client.isActive());
    }
};

QTEST_MAIN(CallAudioClientTest)
#include "tst_call_audio_client.moc"
