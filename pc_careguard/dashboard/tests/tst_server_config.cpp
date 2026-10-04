#include <QtTest>

#include "net/server_config.h"

class ServerConfigTest : public QObject {
    Q_OBJECT

private slots:
    void trailingSlashesAreRemovedSoPathsDoNotBecomeDoubleSlash() {
        QCOMPARE(ServerConfig::normalizeServerUrl("http://10.0.0.5:8000/"), QString("http://10.0.0.5:8000"));
        QCOMPARE(ServerConfig::normalizeServerUrl("http://10.0.0.5:8000///"), QString("http://10.0.0.5:8000"));
    }

    void quotesFromDotEnvAreStripped() {
        QCOMPARE(ServerConfig::normalizeServerUrl("\"http://host:8000\""), QString("http://host:8000"));
        QCOMPARE(ServerConfig::normalizeServerUrl("'http://host:8000/'"), QString("http://host:8000"));
    }

    void whitespaceIsTrimmed() {
        QCOMPARE(ServerConfig::normalizeServerUrl("  http://host:8000 \n"), QString("http://host:8000"));
    }

    void cleanValuesAndEmptyAreLeftAlone() {
        QCOMPARE(ServerConfig::normalizeServerUrl("http://host:8000"), QString("http://host:8000"));
        QCOMPARE(ServerConfig::normalizeServerUrl(""), QString(""));
        QCOMPARE(ServerConfig::normalizeServerUrl("\""), QString("\""));  // 짝이 안 맞는 따옴표는 건드리지 않는다
    }

    void environmentVariableWinsAndIsNormalized() {
        qputenv("CAREGUARD_SERVER_URL", "\"http://from-env:9000/\"");
        QCOMPARE(ServerConfig::resolveServerUrl(), QString("http://from-env:9000"));
        qunsetenv("CAREGUARD_SERVER_URL");
    }

    void fallsBackToDefaultWhenNothingIsConfigured() {
        qunsetenv("CAREGUARD_SERVER_URL");
        qputenv("CAREGUARD_DASHBOARD_DIR", "/nonexistent-dir-for-test");  // .env가 없는 위치
        QCOMPARE(ServerConfig::resolveServerUrl(), QString("http://127.0.0.1:8000"));
        qunsetenv("CAREGUARD_DASHBOARD_DIR");
    }
};

QTEST_MAIN(ServerConfigTest)
#include "tst_server_config.moc"
