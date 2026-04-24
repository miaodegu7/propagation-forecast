#include <QtCore/QAbstractAnimation>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QPropertyAnimation>
#include <QtCore/QTimer>
#include <QtGui/QDesktopServices>
#include <QtGui/QFont>
#include <QtGui/QPixmap>
#include <QtGui/QResizeEvent>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGraphicsOpacityEffect>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

static constexpr int BACKEND_PORT = 18090;

static QString backendUrl(const QString &path) {
    return QStringLiteral("http://127.0.0.1:%1%2").arg(BACKEND_PORT).arg(path);
}

static QFrame *makePanel(const QString &objectName = QString()) {
    auto *panel = new QFrame;
    panel->setObjectName(objectName.isEmpty() ? QStringLiteral("panel") : objectName);
    panel->setFrameShape(QFrame::NoFrame);
    return panel;
}

static QLabel *makeLabel(const QString &text, const QString &objectName = QString()) {
    auto *label = new QLabel(text);
    if (!objectName.isEmpty()) {
        label->setObjectName(objectName);
    }
    label->setWordWrap(true);
    return label;
}

static bool hasBackendTimestamp(const QString &line) {
    return line.size() >= 21
        && line.at(0) == QLatin1Char('[')
        && line.at(5) == QLatin1Char('-')
        && line.at(8) == QLatin1Char('-')
        && line.at(11) == QLatin1Char(' ')
        && line.at(14) == QLatin1Char(':')
        && line.at(17) == QLatin1Char(':')
        && line.at(20) == QLatin1Char(']');
}

class FrontendWindow : public QMainWindow {
public:
    FrontendWindow() {
        appDir = QCoreApplication::applicationDirPath();
        backendPath = QDir(appDir).filePath(QStringLiteral("propagation_bot.exe"));

        setupUi();
        setupConnections();
        applyStyle();

        statusTimer.setInterval(2500);
        QObject::connect(&statusTimer, &QTimer::timeout, [this]() { refreshStatus(); });
        statusTimer.start();

        auto *fade = new QGraphicsOpacityEffect(centralWidget());
        centralWidget()->setGraphicsEffect(fade);
        auto *anim = new QPropertyAnimation(fade, "opacity", this);
        anim->setDuration(260);
        anim->setStartValue(0.0);
        anim->setEndValue(1.0);
        anim->start(QAbstractAnimation::DeleteWhenStopped);

        appendLog(QStringLiteral("Qt 前端已就绪，等待启动后台。"));
        refreshStatus();
    }

private:
    QString appDir;
    QString backendPath;
    QProcess backend;
    QNetworkAccessManager network;
    QTimer statusTimer;
    QPixmap mapPixmap;
    QDateTime lastMqttChatterAt;
    int collapsedMqttChatter = 0;

    QLabel *stateBadge = nullptr;
    QLabel *subtitle = nullptr;
    QLabel *refreshState = nullptr;
    QLabel *lastRefresh = nullptr;
    QLabel *snapshotAge = nullptr;
    QLabel *backendPid = nullptr;
    QLabel *mapPreview = nullptr;
    QPlainTextEdit *logView = nullptr;
    QPushButton *startButton = nullptr;
    QPushButton *stopButton = nullptr;
    QPushButton *openWebButton = nullptr;
    QPushButton *refreshButton = nullptr;
    QPushButton *sendFullButton = nullptr;
    QPushButton *send6mButton = nullptr;
    QPushButton *send2mButton = nullptr;

    void setupUi() {
        setWindowTitle(QStringLiteral("业余无线电传播助手"));
        resize(1120, 720);

        auto *root = new QWidget;
        auto *rootLayout = new QHBoxLayout(root);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->setSpacing(0);

        auto *sidebar = new QFrame;
        sidebar->setObjectName(QStringLiteral("sidebar"));
        sidebar->setFixedWidth(260);
        auto *sideLayout = new QVBoxLayout(sidebar);
        sideLayout->setContentsMargins(22, 24, 22, 24);
        sideLayout->setSpacing(16);

        auto *brand = makeLabel(QStringLiteral("传播助手"), QStringLiteral("brand"));
        auto *brandSub = makeLabel(QStringLiteral("VHF / HF 传播监控与推送控制台"), QStringLiteral("brandSub"));
        stateBadge = makeLabel(QStringLiteral("后台未运行"), QStringLiteral("stateBadge"));

        startButton = new QPushButton(QStringLiteral("启动后台"));
        stopButton = new QPushButton(QStringLiteral("停止后台"));
        openWebButton = new QPushButton(QStringLiteral("打开网页后台"));
        refreshButton = new QPushButton(QStringLiteral("立即刷新"));
        sendFullButton = new QPushButton(QStringLiteral("发送完整简报"));
        send6mButton = new QPushButton(QStringLiteral("发送 6m"));
        send2mButton = new QPushButton(QStringLiteral("发送 2m"));

        for (auto *button : {startButton, stopButton, openWebButton, refreshButton, sendFullButton, send6mButton, send2mButton}) {
            button->setMinimumHeight(38);
            button->setCursor(Qt::PointingHandCursor);
        }

        sideLayout->addWidget(brand);
        sideLayout->addWidget(brandSub);
        sideLayout->addSpacing(8);
        sideLayout->addWidget(stateBadge);
        sideLayout->addSpacing(8);
        sideLayout->addWidget(startButton);
        sideLayout->addWidget(stopButton);
        sideLayout->addWidget(openWebButton);
        sideLayout->addWidget(refreshButton);
        sideLayout->addSpacing(10);
        sideLayout->addWidget(sendFullButton);
        sideLayout->addWidget(send6mButton);
        sideLayout->addWidget(send2mButton);
        sideLayout->addStretch();

        auto *main = new QWidget;
        main->setObjectName(QStringLiteral("main"));
        auto *mainLayout = new QVBoxLayout(main);
        mainLayout->setContentsMargins(28, 26, 28, 24);
        mainLayout->setSpacing(18);

        auto *top = new QHBoxLayout;
        auto *titleBox = new QVBoxLayout;
        auto *title = makeLabel(QStringLiteral("运行面板"), QStringLiteral("title"));
        subtitle = makeLabel(QStringLiteral("本机后台 127.0.0.1:%1").arg(BACKEND_PORT), QStringLiteral("subtitle"));
        titleBox->addWidget(title);
        titleBox->addWidget(subtitle);
        titleBox->setSpacing(4);
        top->addLayout(titleBox);
        top->addStretch();
        backendPid = makeLabel(QStringLiteral("PID -"), QStringLiteral("pid"));
        top->addWidget(backendPid);
        mainLayout->addLayout(top);

        auto *grid = new QGridLayout;
        grid->setSpacing(14);

        auto *statusPanel = makePanel();
        auto *statusLayout = new QVBoxLayout(statusPanel);
        statusLayout->setContentsMargins(18, 16, 18, 16);
        statusLayout->setSpacing(8);
        statusLayout->addWidget(makeLabel(QStringLiteral("刷新状态"), QStringLiteral("panelTitle")));
        refreshState = makeLabel(QStringLiteral("等待连接"), QStringLiteral("metric"));
        lastRefresh = makeLabel(QStringLiteral("最后刷新：-"), QStringLiteral("muted"));
        snapshotAge = makeLabel(QStringLiteral("缓存年龄：-"), QStringLiteral("muted"));
        statusLayout->addWidget(refreshState);
        statusLayout->addWidget(lastRefresh);
        statusLayout->addWidget(snapshotAge);

        auto *actionPanel = makePanel();
        auto *actionLayout = new QVBoxLayout(actionPanel);
        actionLayout->setContentsMargins(18, 16, 18, 16);
        actionLayout->setSpacing(8);
        actionLayout->addWidget(makeLabel(QStringLiteral("操作反馈"), QStringLiteral("panelTitle")));
        actionLayout->addWidget(makeLabel(QStringLiteral("按钮会直接调用本地后台 API；网页后台仍可作为完整设置入口。"), QStringLiteral("muted")));
        actionLayout->addStretch();

        auto *mapPanel = makePanel();
        auto *mapLayout = new QVBoxLayout(mapPanel);
        mapLayout->setContentsMargins(18, 16, 18, 16);
        mapLayout->setSpacing(10);
        mapLayout->addWidget(makeLabel(QStringLiteral("PSK 6m 快照"), QStringLiteral("panelTitle")));
        mapPreview = new QLabel(QStringLiteral("等待图片"));
        mapPreview->setObjectName(QStringLiteral("mapPreview"));
        mapPreview->setAlignment(Qt::AlignCenter);
        mapPreview->setMinimumHeight(320);
        mapPreview->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        mapLayout->addWidget(mapPreview);

        auto *logPanel = makePanel();
        auto *logLayout = new QVBoxLayout(logPanel);
        logLayout->setContentsMargins(18, 16, 18, 16);
        logLayout->setSpacing(10);
        logLayout->addWidget(makeLabel(QStringLiteral("前端日志"), QStringLiteral("panelTitle")));
        logView = new QPlainTextEdit;
        logView->setObjectName(QStringLiteral("logView"));
        logView->setReadOnly(true);
        logView->setMinimumHeight(190);
        logLayout->addWidget(logView);

        grid->addWidget(statusPanel, 0, 0);
        grid->addWidget(actionPanel, 0, 1);
        grid->addWidget(mapPanel, 1, 0, 1, 2);
        grid->addWidget(logPanel, 2, 0, 1, 2);
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 1);
        mainLayout->addLayout(grid);

        rootLayout->addWidget(sidebar);
        rootLayout->addWidget(main, 1);
        setCentralWidget(root);
        updateButtons(false);
    }

    void setupConnections() {
        QObject::connect(startButton, &QPushButton::clicked, [this]() { startBackend(); });
        QObject::connect(stopButton, &QPushButton::clicked, [this]() { stopBackend(); });
        QObject::connect(openWebButton, &QPushButton::clicked, [this]() {
            QDesktopServices::openUrl(QUrl(backendUrl(QStringLiteral("/"))));
        });
        QObject::connect(refreshButton, &QPushButton::clicked, [this]() {
            post(QStringLiteral("/api/refresh?reason=qt"), QByteArray(), QStringLiteral("已请求刷新"));
        });
        QObject::connect(sendFullButton, &QPushButton::clicked, [this]() {
            post(QStringLiteral("/actions/send"), QByteArray("kind=full"), QStringLiteral("已请求发送完整简报"));
        });
        QObject::connect(send6mButton, &QPushButton::clicked, [this]() {
            post(QStringLiteral("/actions/send"), QByteArray("kind=6m"), QStringLiteral("已请求发送 6m 简报"));
        });
        QObject::connect(send2mButton, &QPushButton::clicked, [this]() {
            post(QStringLiteral("/actions/send"), QByteArray("kind=2m"), QStringLiteral("已请求发送 2m 简报"));
        });
        QObject::connect(&backend, &QProcess::readyReadStandardOutput, [this]() {
            appendLog(QString::fromUtf8(backend.readAllStandardOutput()).trimmed());
        });
        QObject::connect(&backend, &QProcess::readyReadStandardError, [this]() {
            appendLog(QString::fromUtf8(backend.readAllStandardError()).trimmed());
        });
        QObject::connect(&backend, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            [this](int code, QProcess::ExitStatus status) {
                appendLog(QStringLiteral("后台已退出，退出码 %1，状态 %2").arg(code).arg(status == QProcess::NormalExit ? QStringLiteral("正常") : QStringLiteral("异常")));
                updateButtons(false);
                stateBadge->setText(QStringLiteral("后台未运行"));
                backendPid->setText(QStringLiteral("PID -"));
            });
    }

    void applyStyle() {
        auto font = QFont(QStringLiteral("Microsoft YaHei UI"), 10);
        QApplication::setFont(font);
        setStyleSheet(QStringLiteral(R"(
            QWidget#main { background: #eef2ee; }
            QFrame#sidebar { background: #16352f; }
            QLabel#brand { color: #f7fff9; font-size: 30px; font-weight: 800; }
            QLabel#brandSub { color: #b9d9ce; font-size: 13px; line-height: 1.4; }
            QLabel#stateBadge { color: #17352f; background: #d9f36a; border-radius: 6px; padding: 8px 10px; font-weight: 700; }
            QPushButton { border: 0; border-radius: 6px; padding: 9px 12px; background: #e9efe9; color: #17352f; font-weight: 700; text-align: left; }
            QPushButton:hover { background: #ffffff; }
            QPushButton:pressed { background: #cddbd3; }
            QPushButton:disabled { color: #83918b; background: #cfd8d3; }
            QLabel#title { color: #17352f; font-size: 28px; font-weight: 800; }
            QLabel#subtitle, QLabel#muted, QLabel.muted { color: #66746e; }
            QLabel#pid { color: #17352f; background: #ffffff; border: 1px solid #d7dfda; border-radius: 6px; padding: 7px 10px; }
            QFrame#panel { background: #ffffff; border: 1px solid #d9e1dc; border-radius: 8px; }
            QLabel#panelTitle { color: #17352f; font-size: 15px; font-weight: 800; }
            QLabel#metric { color: #255c50; font-size: 24px; font-weight: 800; }
            QLabel#mapPreview { background: #f6f8f6; border: 1px dashed #b8c7bf; border-radius: 6px; color: #66746e; }
            QPlainTextEdit#logView { background: #10251f; color: #e9fff6; border: 0; border-radius: 6px; padding: 10px; selection-background-color: #d9f36a; selection-color: #17352f; }
        )"));
    }

    void appendLog(const QString &message) {
        const auto lines = message.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &rawLine : lines) {
            appendLogLine(rawLine.trimmed());
        }
    }

    void appendLogLine(const QString &line) {
        if (line.isEmpty() || shouldCollapseMqttChatter(line)) {
            return;
        }
        flushCollapsedMqttChatter();
        logView->appendPlainText(hasBackendTimestamp(line)
            ? line
            : QStringLiteral("[%1] %2").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), line));
    }

    bool shouldCollapseMqttChatter(const QString &line) {
        const bool isMqttChatter = line.contains(QStringLiteral("PSKReporter MQTT 已断开 rc=7"))
            || line.contains(QStringLiteral("已连接 PSKReporter MQTT"));
        if (!isMqttChatter) {
            return false;
        }

        const QDateTime now = QDateTime::currentDateTime();
        if (lastMqttChatterAt.isValid() && lastMqttChatterAt.secsTo(now) < 20) {
            collapsedMqttChatter++;
            lastMqttChatterAt = now;
            if (collapsedMqttChatter % 6 == 0) {
                logView->appendPlainText(QStringLiteral("[%1] MQTT 连接波动仍在发生，已折叠 %2 条重复日志")
                    .arg(now.toString(QStringLiteral("HH:mm:ss")))
                    .arg(collapsedMqttChatter));
            }
            return true;
        }

        lastMqttChatterAt = now;
        return false;
    }

    void flushCollapsedMqttChatter() {
        if (collapsedMqttChatter <= 0) {
            return;
        }
        logView->appendPlainText(QStringLiteral("[%1] MQTT 连接波动已折叠 %2 条重复日志")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")))
            .arg(collapsedMqttChatter));
        collapsedMqttChatter = 0;
    }

    void updateButtons(bool running) {
        startButton->setEnabled(!running);
        stopButton->setEnabled(running);
        openWebButton->setEnabled(running);
        refreshButton->setEnabled(running);
        sendFullButton->setEnabled(running);
        send6mButton->setEnabled(running);
        send2mButton->setEnabled(running);
    }

    bool backendRunning() const {
        return backend.state() != QProcess::NotRunning;
    }

    void startBackend() {
        if (backendRunning()) {
            appendLog(QStringLiteral("后台已经在运行。"));
            return;
        }
        if (!QFileInfo::exists(backendPath)) {
            appendLog(QStringLiteral("未找到 propagation_bot.exe：%1").arg(backendPath));
            return;
        }
        backend.setWorkingDirectory(appDir);
        backend.setProgram(backendPath);
        backend.setArguments({
            QStringLiteral("--no-browser"),
            QStringLiteral("--hide-console"),
            QStringLiteral("--bind"),
            QStringLiteral("127.0.0.1"),
            QStringLiteral("--port"),
            QString::number(BACKEND_PORT)
        });
        backend.setProcessChannelMode(QProcess::SeparateChannels);
        backend.start();
        if (!backend.waitForStarted(3000)) {
            appendLog(QStringLiteral("后台启动失败：%1").arg(backend.errorString()));
            return;
        }
        appendLog(QStringLiteral("后台已启动，PID %1").arg(backend.processId()));
        stateBadge->setText(QStringLiteral("后台启动中"));
        backendPid->setText(QStringLiteral("PID %1").arg(backend.processId()));
        updateButtons(true);
        QTimer::singleShot(900, [this]() { refreshStatus(); });
    }

    void stopBackend() {
        if (!backendRunning()) {
            updateButtons(false);
            return;
        }
        post(QStringLiteral("/actions/shutdown"), QByteArray(), QStringLiteral("已请求停止后台"));
        if (!backend.waitForFinished(5000)) {
            backend.kill();
            appendLog(QStringLiteral("后台未及时退出，已结束进程。"));
        }
    }

    void refreshStatus() {
        if (!backendRunning()) {
            updateButtons(false);
            return;
        }
        auto reply = network.get(QNetworkRequest(QUrl(backendUrl(QStringLiteral("/api/status")))));
        QObject::connect(reply, &QNetworkReply::finished, [this, reply]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                stateBadge->setText(QStringLiteral("等待后台"));
                refreshState->setText(QStringLiteral("连接中"));
                return;
            }
            const auto doc = QJsonDocument::fromJson(reply->readAll());
            const auto obj = doc.object();
            const bool refreshing = obj.value(QStringLiteral("refreshing")).toBool();
            const QString status = obj.value(QStringLiteral("status")).toString(QStringLiteral("空闲"));
            stateBadge->setText(refreshing ? QStringLiteral("刷新中") : QStringLiteral("后台运行中"));
            refreshState->setText(status);
            lastRefresh->setText(QStringLiteral("最后刷新：%1").arg(obj.value(QStringLiteral("last_refreshed_text")).toString(QStringLiteral("-"))));
            snapshotAge->setText(QStringLiteral("缓存年龄：%1 秒").arg(obj.value(QStringLiteral("snapshot_age_seconds")).toInt(-1)));
            updateButtons(true);
            loadMapPreview();
        });
    }

    void loadMapPreview() {
        auto reply = network.get(QNetworkRequest(QUrl(backendUrl(QStringLiteral("/api/pskmap.png?band=6m")))));
        QObject::connect(reply, &QNetworkReply::finished, [this, reply]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                mapPreview->setText(QStringLiteral("图片暂不可用"));
                return;
            }
            QPixmap pixmap;
            if (pixmap.loadFromData(reply->readAll())) {
                mapPixmap = pixmap;
                updateMapPreviewPixmap();
            }
        });
    }

    void updateMapPreviewPixmap() {
        if (mapPixmap.isNull() || !mapPreview) {
            return;
        }
        const QSize target = mapPreview->contentsRect().size();
        if (target.isEmpty()) {
            return;
        }

        QPixmap scaled = mapPixmap.scaled(target, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        const int x = qMax(0, (scaled.width() - target.width()) / 2);
        const int y = qMax(0, (scaled.height() - target.height()) / 2);
        mapPreview->setPixmap(scaled.copy(x, y, target.width(), target.height()));
    }

    void post(const QString &path, const QByteArray &body, const QString &successMessage) {
        QNetworkRequest request(QUrl(backendUrl(path)));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
        auto reply = network.post(request, body);
        QObject::connect(reply, &QNetworkReply::finished, [this, reply, successMessage]() {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const auto error = reply->error();
            reply->deleteLater();
            if (error == QNetworkReply::NoError || (status >= 200 && status < 400)) {
                appendLog(successMessage);
                QTimer::singleShot(1200, [this]() { refreshStatus(); });
            } else {
                appendLog(QStringLiteral("请求失败，HTTP %1：%2").arg(status).arg(reply->errorString()));
            }
        });
    }

protected:
    void resizeEvent(QResizeEvent *event) override {
        QMainWindow::resizeEvent(event);
        updateMapPreviewPixmap();
    }
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    FrontendWindow window;
    window.show();
    return app.exec();
}
