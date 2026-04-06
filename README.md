# Propagation Forecast Bot

这是一个面向业余无线电的服务器端机器人，使用 C 编写，提供：

- `HAMqsl` 太阳与 HF/VHF 传播数据抓取
- `PSKReporter MQTT` 的 6m 实时 spot 监测
- `Open-Meteo` 的本地天气与日照信息辅助分析
- `OneBot 11 / NapCatQQ` 的 QQ 群聊与私聊推送
- 内置网页管理后台
- 每天两次定时发送与按需问答回复

## 依赖

以 Debian / Ubuntu 为例：

```bash
sudo apt update
sudo apt install -y build-essential libcurl4-openssl-dev libsqlite3-dev libmosquitto-dev
```

## 编译

```bash
make
```

## 运行

```bash
./propagation_bot ./propagation.db
```

首次启动会自动创建数据库并写入默认设置，默认后台监听：

- `http://0.0.0.0:8080/`

## 后台配置

启动后进入网页后台，至少需要配置这些项目：

- 台站呼号
- 台站网格
- 纬度 / 经度
- OneBot API 地址，例如 `http://127.0.0.1:3000`
- OneBot Access Token
- OneBot 回调令牌
- 两个定时发送时间
- 目标群号 / QQ 号

## NapCat / OneBot 11 建议配置

- HTTP API 地址：`http://127.0.0.1:3000`
- 反向 HTTP 上报地址：`http://你的服务器:8080/api/onebot?token=你设置的令牌`
- Access Token：与后台配置保持一致

程序会：

- 主动调用 `send_group_msg` / `send_private_msg`
- 接收群聊或私聊消息，在有人发送“传播”“6m”“太阳”“help”等关键词时即时回复

## systemd 示例

```ini
[Unit]
Description=Ham Propagation Forecast Bot
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/opt/propagation-bot
ExecStart=/opt/propagation-bot/propagation_bot /opt/propagation-bot/propagation.db
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

仓库里也提供了现成文件：

- `deploy/propagation-bot.service.example`

## 说明

- `HAMqsl` 提供太阳活动、HF 段况和部分 VHF 条件
- `PSKReporter` 的网页查询接口当前常被 Cloudflare challenge 阻挡，因此本项目使用实时 `MQTT` spot 流判断 6m
- 气象部分只作为 6m 辅助判断，优先级低于实时 spot 与太阳地磁数据
