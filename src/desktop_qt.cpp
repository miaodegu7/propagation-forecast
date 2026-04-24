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
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStyle>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

static constexpr int BACKEND_PORT = 18090;

static QString zh(const char *text) {
    return QString::fromUtf8(text);
}

static QString backendUrl(const QString &path) {
    return QStringLiteral("http://127.0.0.1:%1%2").arg(BACKEND_PORT).arg(path);
}

static void repolish(QWidget *widget) {
    if (!widget) {
        return;
    }
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

static void setTone(QWidget *widget, const char *tone) {
    if (!widget) {
        return;
    }
    widget->setProperty("tone", tone);
    repolish(widget);
}

static QFrame *makePanel(const QString &objectName) {
    auto *panel = new QFrame;
    panel->setObjectName(objectName);
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

static QPushButton *makeButton(const QString &text, const char *tone) {
    auto *button = new QPushButton(text);
    button->setCursor(Qt::PointingHandCursor);
    button->setMinimumHeight(48);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    button->setProperty("tone", tone);
    return button;
}

static QLabel *makePill(const QString &text, const char *tone) {
    auto *pill = makeLabel(text, QStringLiteral("statusPill"));
    pill->setAlignment(Qt::AlignCenter);
    pill->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setTone(pill, tone);
    return pill;
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

class PreviewLabel : public QLabel {
public:
    PreviewLabel() {
        setAlignment(Qt::AlignCenter);
        setMinimumHeight(320);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setPreviewPixmap(const QPixmap &pixmap) {
        sourcePixmap = pixmap;
        QLabel::clear();
        updatePreview();
    }

    void setPlaceholderText(const QString &text) {
        sourcePixmap = QPixmap();
        QLabel::clear();
        setText(text);
    }

protected:
    void resizeEvent(QResizeEvent *event) override {
        QLabel::resizeEvent(event);
        updatePreview();
    }

private:
    QPixmap sourcePixmap;

    void updatePreview() {
        if (sourcePixmap.isNull()) {
            return;
        }
        const QSize target = contentsRect().size();
        if (target.isEmpty()) {
            return;
        }
        QLabel::setPixmap(sourcePixmap.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
};

class FrontendWindow : public QMainWindow {
public:
    FrontendWindow() {
        appDir = QCoreApplication::applicationDirPath();
        backendPath = QDir(appDir).filePath(QStringLiteral("propagation_bot.exe"));

        setupUi();
        setupConnections();
        applyStyle();

        statusTimer.setInterval(2500);
        statusTimer.setSingleShot(false);

        stopTimer.setInterval(12000);
        stopTimer.setSingleShot(true);
        QObject::connect(&stopTimer, &QTimer::timeout, [this]() {
            if (!backendRunning()) {
                return;
            }
            forceKilled = true;
            appendLog(zh(u8"后台停止超时，已强制结束进程。"));
            backend.kill();
        });

        auto *fade = new QGraphicsOpacityEffect(centralWidget());
        centralWidget()->setGraphicsEffect(fade);
        auto *anim = new QPropertyAnimation(fade, "opacity", this);
        anim->setDuration(260);
        anim->setStartValue(0.0);
        anim->setEndValue(1.0);
        anim->start(QAbstractAnimation::DeleteWhenStopped);

        appendLog(zh(u8"Qt 前端已就绪，等待启动后台。"));
        setBackendStateUi(zh(u8"后台未运行"), "idle");
        setStatusPill(servicePill, zh(u8"服务未启动"), "idle");
        setStatusPill(refreshPill, zh(u8"等待后台"), "idle");
        setStatusPill(cachePill, zh(u8"缓存未建立"), "warn");
        setStatusPill(resultPill, zh(u8"尚未刷新"), "idle");
        refreshStatus();
    }

private:
    QString appDir;
    QString backendPath;
    QProcess backend;
    QNetworkAccessManager network;
    QTimer statusTimer;
    QTimer stopTimer;
    int collapsedMqttChatter = 0;
    bool stopRequested = false;
    bool forceKilled = false;

    QLabel *stateBadge = nullptr;
    QLabel *backendPid = nullptr;
    QLabel *refreshState = nullptr;
    QLabel *lastRefresh = nullptr;
    QLabel *snapshotAge = nullptr;
    QLabel *subtitle = nullptr;
    QLabel *servicePill = nullptr;
    QLabel *refreshPill = nullptr;
    QLabel *cachePill = nullptr;
    QLabel *resultPill = nullptr;
    PreviewLabel *mapPreview = nullptr;
    QPlainTextEdit *logView = nullptr;

    QPushButton *startButton = nullptr;
    QPushButton *stopButton = nullptr;
    QPushButton *openWebButton = nullptr;
    QPushButton *refreshButton = nullptr;
    QPushButton *sendFullButton = nullptr;
    QPushButton *send6mButton = nullptr;
    QPushButton *send2mButton = nullptr;

    void setupUi() {
        setWindowTitle(zh(u8"业余无线电传播助手"));
        resize(1040, 680);
        setMinimumSize(860, 560);

        auto *root = new QWidget;
        root->setObjectName(QStringLiteral("window"));
        auto *rootLayout = new QVBoxLayout(root);
        rootLayout->setContentsMargins(10, 10, 10, 10);

        auto *shell = makePanel(QStringLiteral("shell"));
        auto *shellLayout = new QHBoxLayout(shell);
        shellLayout->setContentsMargins(12, 12, 12, 12);

        auto *rootSplitter = new QSplitter(Qt::Horizontal);
        rootSplitter->setObjectName(QStringLiteral("rootSplitter"));
        rootSplitter->setChildrenCollapsible(false);
        rootSplitter->setHandleWidth(8);

        auto *sidebar = makePanel(QStringLiteral("sidebar"));
        sidebar->setMinimumWidth(280);
        auto *sideLayout = new QVBoxLayout(sidebar);
        sideLayout->setContentsMargins(18, 18, 18, 18);
        sideLayout->setSpacing(14);

        auto *brandPanel = makePanel(QStringLiteral("brandPanel"));
        auto *brandLayout = new QVBoxLayout(brandPanel);
        brandLayout->setContentsMargins(18, 18, 18, 18);
        brandLayout->setSpacing(6);
        brandLayout->addWidget(makeLabel(zh(u8"传播助手"), QStringLiteral("brand")));
        brandLayout->addWidget(makeLabel(zh(u8"本地后台控制与传播快览"), QStringLiteral("brandSub")));

        auto *servicePanel = makePanel(QStringLiteral("sidebarCard"));
        auto *serviceLayout = new QVBoxLayout(servicePanel);
        serviceLayout->setContentsMargins(16, 16, 16, 16);
        serviceLayout->setSpacing(12);
        serviceLayout->addWidget(makeLabel(zh(u8"后台控制"), QStringLiteral("sidebarTitle")));

        stateBadge = makeLabel(zh(u8"后台未运行"), QStringLiteral("stateBadge"));
        stateBadge->setAlignment(Qt::AlignCenter);
        backendPid = makeLabel(QStringLiteral("PID -"), QStringLiteral("pidBadge"));
        backendPid->setAlignment(Qt::AlignCenter);
        serviceLayout->addWidget(stateBadge);
        serviceLayout->addWidget(backendPid);

        startButton = makeButton(zh(u8"启动后台"), "accent");
        stopButton = makeButton(zh(u8"停止后台"), "danger");
        openWebButton = makeButton(zh(u8"打开网页后台"), "soft");
        refreshButton = makeButton(zh(u8"立即刷新"), "soft");

        auto *serviceGrid = new QGridLayout;
        serviceGrid->setHorizontalSpacing(10);
        serviceGrid->setVerticalSpacing(10);
        serviceGrid->addWidget(startButton, 0, 0);
        serviceGrid->addWidget(stopButton, 0, 1);
        serviceGrid->addWidget(openWebButton, 1, 0);
        serviceGrid->addWidget(refreshButton, 1, 1);
        serviceLayout->addLayout(serviceGrid);

        auto *sendPanel = makePanel(QStringLiteral("sidebarCard"));
        auto *sendLayout = new QVBoxLayout(sendPanel);
        sendLayout->setContentsMargins(16, 16, 16, 16);
        sendLayout->setSpacing(12);
        sendLayout->addWidget(makeLabel(zh(u8"快捷推送"), QStringLiteral("sidebarTitle")));

        sendFullButton = makeButton(zh(u8"发送完整简报"), "accentLight");
        send6mButton = makeButton(zh(u8"发送 6m 简报"), "soft");
        send2mButton = makeButton(zh(u8"发送 2m 简报"), "soft");

        auto *sendGrid = new QGridLayout;
        sendGrid->setHorizontalSpacing(10);
        sendGrid->setVerticalSpacing(10);
        sendGrid->addWidget(sendFullButton, 0, 0, 1, 2);
        sendGrid->addWidget(send6mButton, 1, 0);
        sendGrid->addWidget(send2mButton, 1, 1);
        sendLayout->addLayout(sendGrid);

        sideLayout->addWidget(brandPanel);
        sideLayout->addWidget(servicePanel);
        sideLayout->addWidget(sendPanel);
        sideLayout->addStretch();

        auto *workspace = new QWidget;
        auto *workspaceLayout = new QVBoxLayout(workspace);
        workspaceLayout->setContentsMargins(0, 0, 0, 0);

        auto *mainSplitter = new QSplitter(Qt::Vertical);
        mainSplitter->setObjectName(QStringLiteral("mainSplitter"));
        mainSplitter->setChildrenCollapsible(false);
        mainSplitter->setHandleWidth(8);

        auto *topArea = new QWidget;
        auto *topLayout = new QVBoxLayout(topArea);
        topLayout->setContentsMargins(0, 0, 0, 0);
        topLayout->setSpacing(12);

        auto *heroPanel = makePanel(QStringLiteral("heroPanel"));
        auto *heroLayout = new QVBoxLayout(heroPanel);
        heroLayout->setContentsMargins(20, 18, 20, 18);
        heroLayout->setSpacing(14);

        auto *heroTop = new QHBoxLayout;
        auto *heroTitleBox = new QVBoxLayout;
        heroTitleBox->setSpacing(4);
        heroTitleBox->addWidget(makeLabel(zh(u8"运行控制台"), QStringLiteral("title")));
        subtitle = makeLabel(QStringLiteral("127.0.0.1:%1").arg(BACKEND_PORT), QStringLiteral("subtitle"));
        heroTitleBox->addWidget(subtitle);
        heroTop->addLayout(heroTitleBox);
        heroTop->addStretch();
        heroTop->addWidget(stateBadge);
        heroLayout->addLayout(heroTop);

        auto *pillLayout = new QHBoxLayout;
        pillLayout->setSpacing(10);
        servicePill = makePill(zh(u8"服务未启动"), "idle");
        refreshPill = makePill(zh(u8"等待后台"), "idle");
        cachePill = makePill(zh(u8"缓存未建立"), "warn");
        resultPill = makePill(zh(u8"尚未刷新"), "idle");
        pillLayout->addWidget(servicePill);
        pillLayout->addWidget(refreshPill);
        pillLayout->addWidget(cachePill);
        pillLayout->addWidget(resultPill);
        heroLayout->addLayout(pillLayout);

        auto *metricsLayout = new QGridLayout;
        metricsLayout->setHorizontalSpacing(12);
        metricsLayout->setVerticalSpacing(12);
        metricsLayout->addWidget(makeMetricCard(zh(u8"刷新状态"), &refreshState, zh(u8"等待连接")), 0, 0);
        metricsLayout->addWidget(makeMetricCard(zh(u8"最后刷新"), &lastRefresh, QStringLiteral("-")), 0, 1);
        metricsLayout->addWidget(makeMetricCard(zh(u8"快照缓存"), &snapshotAge, QStringLiteral("-")), 0, 2);
        heroLayout->addLayout(metricsLayout);

        auto *mapPanel = makePanel(QStringLiteral("mapPanel"));
        auto *mapLayout = new QVBoxLayout(mapPanel);
        mapLayout->setContentsMargins(16, 16, 16, 16);
        mapLayout->setSpacing(10);
        mapLayout->addWidget(makeLabel(zh(u8"PSK 6m 快照"), QStringLiteral("panelTitle")));

        mapPreview = new PreviewLabel;
        mapPreview->setObjectName(QStringLiteral("mapPreview"));
        mapPreview->setPlaceholderText(zh(u8"等待图片"));
        mapLayout->addWidget(mapPreview, 1);

        topLayout->addWidget(heroPanel);
        topLayout->addWidget(mapPanel, 1);

        auto *logPanel = makePanel(QStringLiteral("logPanel"));
        auto *logLayout = new QVBoxLayout(logPanel);
        logLayout->setContentsMargins(16, 16, 16, 16);
        logLayout->setSpacing(10);
        logLayout->addWidget(makeLabel(zh(u8"运行日志"), QStringLiteral("panelTitle")));
        logView = new QPlainTextEdit;
        logView->setObjectName(QStringLiteral("logView"));
        logView->setReadOnly(true);
        logView->setMinimumHeight(150);
        logLayout->addWidget(logView);

        mainSplitter->addWidget(topArea);
        mainSplitter->addWidget(logPanel);
        mainSplitter->setStretchFactor(0, 5);
        mainSplitter->setStretchFactor(1, 2);
        mainSplitter->setSizes({480, 180});

        workspaceLayout->addWidget(mainSplitter);

        rootSplitter->addWidget(sidebar);
        rootSplitter->addWidget(workspace);
        rootSplitter->setStretchFactor(0, 0);
        rootSplitter->setStretchFactor(1, 1);
        rootSplitter->setSizes({320, 720});

        shellLayout->addWidget(rootSplitter);
        rootLayout->addWidget(shell);
        setCentralWidget(root);

        updateButtons(false);
    }

    QFrame *makeMetricCard(const QString &caption, QLabel **valueLabel, const QString &initialText) {
        auto *card = makePanel(QStringLiteral("metricCard"));
        auto *layout = new QVBoxLayout(card);
        layout->setContentsMargins(16, 14, 16, 14);
        layout->setSpacing(6);
        layout->addWidget(makeLabel(caption, QStringLiteral("metricCaption")));
        auto *value = makeLabel(initialText, QStringLiteral("metricValue"));
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(value);
        *valueLabel = value;
        return card;
    }

    void setupConnections() {
        QObject::connect(&statusTimer, &QTimer::timeout, [this]() { refreshStatus(); });

        QObject::connect(startButton, &QPushButton::clicked, [this]() { startBackend(); });
        QObject::connect(stopButton, &QPushButton::clicked, [this]() { stopBackend(); });
        QObject::connect(openWebButton, &QPushButton::clicked, [this]() {
            QDesktopServices::openUrl(QUrl(backendUrl(QStringLiteral("/"))));
        });
        QObject::connect(refreshButton, &QPushButton::clicked, [this]() {
            post(QStringLiteral("/api/refresh?reason=qt"), QByteArray(), zh(u8"已请求刷新后台。"), true);
        });
        QObject::connect(sendFullButton, &QPushButton::clicked, [this]() {
            post(QStringLiteral("/actions/send"), QByteArray("kind=full"), zh(u8"已请求发送完整简报。"), true);
        });
        QObject::connect(send6mButton, &QPushButton::clicked, [this]() {
            post(QStringLiteral("/actions/send"), QByteArray("kind=6m"), zh(u8"已请求发送 6m 简报。"), true);
        });
        QObject::connect(send2mButton, &QPushButton::clicked, [this]() {
            post(QStringLiteral("/actions/send"), QByteArray("kind=2m"), zh(u8"已请求发送 2m 简报。"), true);
        });

        QObject::connect(&backend, &QProcess::readyReadStandardOutput, [this]() {
            appendLog(QString::fromUtf8(backend.readAllStandardOutput()));
        });
        QObject::connect(&backend, &QProcess::readyReadStandardError, [this]() {
            appendLog(QString::fromUtf8(backend.readAllStandardError()));
        });
        QObject::connect(&backend, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            [this](int code, QProcess::ExitStatus status) {
                flushCollapsedMqttChatter();
                stopTimer.stop();
                statusTimer.stop();

                const bool stoppedByUser = stopRequested;
                const bool killedByTimeout = forceKilled;
                stopRequested = false;
                forceKilled = false;

                backendPid->setText(QStringLiteral("PID -"));
                refreshState->setText(zh(u8"等待连接"));
                updateButtons(false);
                setBackendStateUi(zh(u8"后台未运行"), "idle");
                setStatusPill(servicePill, zh(u8"服务未启动"), "idle");
                setStatusPill(refreshPill, zh(u8"等待后台"), "idle");

                if (killedByTimeout) {
                    appendLog(zh(u8"后台已被强制结束。"));
                    setStatusPill(resultPill, zh(u8"停止超时"), "error");
                } else if (stoppedByUser) {
                    appendLog(zh(u8"后台已停止。"));
                    setStatusPill(resultPill, zh(u8"已手动停止"), "idle");
                } else {
                    appendLog(zh(u8"后台已退出，退出码 %1，状态 %2")
                        .arg(code)
                        .arg(status == QProcess::NormalExit ? zh(u8"正常") : zh(u8"异常")));
                    setStatusPill(resultPill, zh(u8"后台已退出"), status == QProcess::NormalExit ? "warn" : "error");
                }
            });
    }

    void applyStyle() {
        auto baseFont = QFont(QStringLiteral("Microsoft YaHei UI"), 10);
        QApplication::setFont(baseFont);

        setStyleSheet(QStringLiteral(R"(
            QWidget#window {
                background: qradialgradient(cx:0.15, cy:0.08, radius:1.1, fx:0.15, fy:0.08,
                    stop:0 #eef5ef, stop:0.55 #dde7e1, stop:1 #cbd7d0);
            }
            QFrame#shell {
                background: rgba(255, 255, 255, 0.55);
                border: 1px solid rgba(23, 53, 47, 0.10);
                border-radius: 22px;
            }
            QSplitter::handle {
                background: transparent;
            }
            QFrame#sidebar {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                    stop:0 #18362f, stop:1 #244740);
                border: 1px solid rgba(255, 255, 255, 0.08);
                border-radius: 18px;
            }
            QFrame#brandPanel, QFrame#sidebarCard, QFrame#heroPanel, QFrame#mapPanel, QFrame#logPanel, QFrame#metricCard {
                background: rgba(255, 255, 255, 0.96);
                border: 1px solid #d9e2dd;
                border-radius: 14px;
            }
            QLabel#brand {
                color: #102c25;
                font-size: 30px;
                font-weight: 900;
                letter-spacing: 1px;
            }
            QLabel#brandSub {
                color: #4f665f;
                font-size: 13px;
            }
            QLabel#sidebarTitle {
                color: #183a33;
                font-size: 15px;
                font-weight: 800;
            }
            QLabel#subtitle, QLabel#metricCaption {
                color: #6a7b75;
                font-size: 12px;
            }
            QLabel#title {
                color: #17352f;
                font-size: 30px;
                font-weight: 900;
            }
            QLabel#panelTitle {
                color: #17352f;
                font-size: 16px;
                font-weight: 800;
            }
            QLabel#stateBadge, QLabel#statusPill {
                border-radius: 15px;
                padding: 10px 14px;
                font-weight: 800;
                border: 1px solid transparent;
            }
            QLabel#stateBadge[tone="idle"], QLabel#statusPill[tone="idle"] {
                background: #ecf1ee;
                color: #4c625b;
                border-color: #d7dfda;
            }
            QLabel#stateBadge[tone="running"], QLabel#statusPill[tone="running"] {
                background: #e3f2d4;
                color: #26452f;
                border-color: #cfe3bb;
            }
            QLabel#stateBadge[tone="accent"], QLabel#statusPill[tone="accent"] {
                background: #d9f36a;
                color: #17352f;
                border-color: #c8e557;
            }
            QLabel#stateBadge[tone="warn"], QLabel#statusPill[tone="warn"] {
                background: #f7ecd6;
                color: #735126;
                border-color: #ecd9b5;
            }
            QLabel#stateBadge[tone="error"], QLabel#statusPill[tone="error"] {
                background: #f6e2de;
                color: #813f31;
                border-color: #ebc4bc;
            }
            QLabel#pidBadge {
                background: #edf3ef;
                color: #35554c;
                border-radius: 14px;
                padding: 9px 12px;
                border: 1px solid #d3ddd7;
            }
            QLabel#metricValue {
                color: #17352f;
                font-size: 22px;
                font-weight: 800;
            }
            QLabel#mapPreview {
                background: #f8fbf8;
                border: 1px solid #d7e0da;
                border-radius: 10px;
                color: #677772;
                padding: 6px;
            }
            QPlainTextEdit#logView {
                background: #10251f;
                color: #ecfff6;
                border: 0;
                border-radius: 10px;
                padding: 10px;
                selection-background-color: #d9f36a;
                selection-color: #17352f;
            }
            QPushButton {
                border: 0;
                border-radius: 16px;
                padding: 12px 16px;
                font-weight: 800;
                text-align: center;
            }
            QPushButton[tone="accent"] {
                background: #d9f36a;
                color: #17352f;
            }
            QPushButton[tone="accentLight"] {
                background: #eff7cb;
                color: #17352f;
                border: 1px solid #dce8a3;
            }
            QPushButton[tone="soft"] {
                background: #edf2ef;
                color: #17352f;
                border: 1px solid #d8e1db;
            }
            QPushButton[tone="danger"] {
                background: #f6e7df;
                color: #7b3d21;
                border: 1px solid #ebcfbf;
            }
            QPushButton:hover {
                background: #ffffff;
            }
            QPushButton:pressed {
                background: #d6ded8;
            }
            QPushButton:disabled {
                background: #d5ddd8;
                color: #80908a;
                border: 1px solid #cfd8d2;
            }
        )"));
    }

    void setBackendStateUi(const QString &text, const char *tone) {
        stateBadge->setText(text);
        setTone(stateBadge, tone);
    }

    void setStatusPill(QLabel *pill, const QString &text, const char *tone) {
        if (!pill) {
            return;
        }
        pill->setText(text);
        setTone(pill, tone);
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
        const bool isMqttChatter = line.contains(zh(u8"PSKReporter MQTT 已断开 rc=7"))
            || line.contains(zh(u8"已连接 PSKReporter MQTT"));
        if (!isMqttChatter) {
            return false;
        }
        collapsedMqttChatter++;
        return true;
    }

    void flushCollapsedMqttChatter() {
        if (collapsedMqttChatter >= 6) {
            logView->appendPlainText(QStringLiteral("[%1] %2")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")))
                .arg(zh(u8"MQTT 连接波动已折叠 %1 条重复日志").arg(collapsedMqttChatter)));
        }
        collapsedMqttChatter = 0;
    }

    void updateButtons(bool running) {
        const bool interactive = running && !stopRequested;
        startButton->setEnabled(!running && !stopRequested);
        stopButton->setEnabled(running && !stopRequested);
        openWebButton->setEnabled(interactive);
        refreshButton->setEnabled(interactive);
        sendFullButton->setEnabled(interactive);
        send6mButton->setEnabled(interactive);
        send2mButton->setEnabled(interactive);
    }

    bool backendRunning() const {
        return backend.state() != QProcess::NotRunning;
    }

    void startBackend() {
        if (backendRunning()) {
            appendLog(zh(u8"后台已经在运行。"));
            return;
        }
        if (!QFileInfo::exists(backendPath)) {
            appendLog(zh(u8"未找到 propagation_bot.exe：%1").arg(backendPath));
            return;
        }

        flushCollapsedMqttChatter();
        stopRequested = false;
        forceKilled = false;
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
            appendLog(zh(u8"后台启动失败：%1").arg(backend.errorString()));
            setBackendStateUi(zh(u8"启动失败"), "error");
            setStatusPill(servicePill, zh(u8"服务启动失败"), "error");
            return;
        }

        backendPid->setText(QStringLiteral("PID %1").arg(backend.processId()));
        refreshState->setText(zh(u8"连接中"));
        updateButtons(true);
        statusTimer.start();
        setBackendStateUi(zh(u8"后台启动中"), "accent");
        setStatusPill(servicePill, zh(u8"服务启动中"), "accent");
        setStatusPill(refreshPill, zh(u8"等待状态"), "idle");
        appendLog(zh(u8"后台已启动，PID %1").arg(backend.processId()));
        QTimer::singleShot(900, [this]() {
            if (!stopRequested && backendRunning()) {
                refreshStatus();
            }
        });
    }

    void stopBackend() {
        if (!backendRunning()) {
            setBackendStateUi(zh(u8"后台未运行"), "idle");
            updateButtons(false);
            return;
        }

        stopRequested = true;
        forceKilled = false;
        statusTimer.stop();
        updateButtons(true);
        setBackendStateUi(zh(u8"后台停止中"), "warn");
        setStatusPill(servicePill, zh(u8"服务停止中"), "warn");
        setStatusPill(refreshPill, zh(u8"正在结束"), "warn");
        refreshState->setText(zh(u8"正在结束"));
        appendLog(zh(u8"已请求停止后台。"));

        QNetworkRequest request(QUrl(backendUrl(QStringLiteral("/actions/shutdown"))));
        auto reply = network.post(request, QByteArray());
        QObject::connect(reply, &QNetworkReply::finished, [this, reply]() {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const auto error = reply->error();
            const QString errorText = reply->errorString();
            reply->deleteLater();
            if (!stopRequested) {
                return;
            }
            if (error == QNetworkReply::NoError || (status >= 200 && status < 400)) {
                appendLog(zh(u8"后台已收到停止指令。"));
            } else if (backendRunning()) {
                appendLog(zh(u8"停止指令未确认：%1").arg(errorText));
            }
        });

        stopTimer.start();
    }

    void refreshStatus() {
        if (!backendRunning() || stopRequested) {
            updateButtons(false);
            return;
        }

        auto reply = network.get(QNetworkRequest(QUrl(backendUrl(QStringLiteral("/api/status")))));
        QObject::connect(reply, &QNetworkReply::finished, [this, reply]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                if (stopRequested || !backendRunning()) {
                    return;
                }
                setBackendStateUi(zh(u8"等待后台"), "idle");
                setStatusPill(servicePill, zh(u8"连接中"), "idle");
                setStatusPill(refreshPill, zh(u8"状态未返回"), "warn");
                return;
            }

            const auto doc = QJsonDocument::fromJson(reply->readAll());
            const auto obj = doc.object();
            const bool refreshing = obj.value(QStringLiteral("refreshing")).toBool();
            const int lastRc = obj.value(QStringLiteral("last_rc")).toInt(0);
            const QString status = obj.value(QStringLiteral("status")).toString(zh(u8"空闲"));
            const QString reason = obj.value(QStringLiteral("reason")).toString();
            const QString refreshedText = obj.value(QStringLiteral("last_refreshed_text")).toString(QStringLiteral("-"));
            const int ageSeconds = obj.value(QStringLiteral("snapshot_age_seconds")).toInt(-1);

            setBackendStateUi(refreshing ? zh(u8"正在刷新") : zh(u8"后台运行中"), refreshing ? "accent" : "running");
            setStatusPill(servicePill, zh(u8"服务在线"), "running");
            setStatusPill(refreshPill,
                refreshing
                    ? zh(u8"刷新中 · %1").arg(reason.isEmpty() ? QStringLiteral("manual") : reason)
                    : zh(u8"状态 · %1").arg(status),
                refreshing ? "accent" : (lastRc < 0 ? "warn" : "running"));
            setStatusPill(cachePill,
                ageSeconds < 0 ? zh(u8"缓存未建立") : zh(u8"缓存 %1 秒").arg(ageSeconds),
                ageSeconds < 0 ? "warn" : (ageSeconds > 600 ? "warn" : "running"));
            setStatusPill(resultPill,
                lastRc < 0 ? zh(u8"最近刷新失败") : zh(u8"最后刷新 · %1").arg(refreshedText),
                lastRc < 0 ? "error" : "idle");

            refreshState->setText(status);
            lastRefresh->setText(refreshedText);
            snapshotAge->setText(ageSeconds < 0 ? zh(u8"暂无") : zh(u8"%1 秒").arg(ageSeconds));
            backendPid->setText(QStringLiteral("PID %1").arg(backend.processId()));
            updateButtons(true);
            loadMapPreview();
        });
    }

    void loadMapPreview() {
        if (!backendRunning() || stopRequested) {
            return;
        }

        auto reply = network.get(QNetworkRequest(QUrl(backendUrl(QStringLiteral("/api/pskmap.png?band=6m")))));
        QObject::connect(reply, &QNetworkReply::finished, [this, reply]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                if (!stopRequested && backendRunning()) {
                    mapPreview->setPlaceholderText(zh(u8"图片暂不可用"));
                }
                return;
            }
            QPixmap pixmap;
            if (pixmap.loadFromData(reply->readAll())) {
                mapPreview->setPreviewPixmap(pixmap);
            }
        });
    }

    void post(const QString &path, const QByteArray &body, const QString &successMessage, bool refreshAfterSuccess) {
        if (!backendRunning() || stopRequested) {
            appendLog(zh(u8"后台未运行，无法发送请求。"));
            return;
        }

        QNetworkRequest request(QUrl(backendUrl(path)));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
        auto reply = network.post(request, body);
        QObject::connect(reply, &QNetworkReply::finished, [this, reply, successMessage, refreshAfterSuccess]() {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const auto error = reply->error();
            const QString errorText = reply->errorString();
            reply->deleteLater();

            if (error == QNetworkReply::NoError || (status >= 200 && status < 400)) {
                appendLog(successMessage);
                if (refreshAfterSuccess) {
                    QTimer::singleShot(1000, [this]() {
                        if (!stopRequested && backendRunning()) {
                            refreshStatus();
                        }
                    });
                }
                return;
            }

            if (stopRequested || !backendRunning()) {
                return;
            }
            appendLog(zh(u8"请求失败，HTTP %1：%2").arg(status).arg(errorText));
        });
    }
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    FrontendWindow window;
    window.show();
    return app.exec();
}
