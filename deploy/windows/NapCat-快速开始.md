# NapCat 快速开始

这份说明按“更像真人 QQ 一样长期运行”的方式整理，适合直接配合本项目使用。

## 你需要准备

1. 一个专门用于机器人的 QQ 号
2. 已安装并能登录的 `NapCat`
3. 本程序的 `propagation_bot.exe` 或单文件版 `propagation_bot_single.exe`

如果你用的是单文件版：

- 直接双击 `propagation_bot_single.exe`
- 程序会自动在浏览器打开后台控制台
- 默认数据库会放在 `exe` 同目录下

## 第一步：先启动程序生成数据库

在程序目录运行：

```bat
propagation_bot.exe runtime\propagation.db
```

首次启动会自动创建数据库。

默认后台地址：

- `http://127.0.0.1:8080/`

如果你还没设置后台账号密码，就可以直接打开。

## 第二步：先在后台把关键参数填好

先填这些：

- `台站呼号`
- `主网格`
- `PSK 监控网格`
- `时区`
- `OneBot API 地址`
- `OneBot Access Token`
- `回调令牌`

推荐值：

- `OneBot API 地址`：`http://127.0.0.1:3000`
- `群发间隔(ms)`：`1200`
- `失败重试次数`：`1`
- `重试等待(ms)`：`2500`
- `每分钟问答限额`：`6`

## 第三步：配置 NapCat

在 NapCat 里开启：

1. `HTTP API`
2. `反向 HTTP 上报`

推荐配置：

- HTTP API 地址：`http://127.0.0.1:3000`
- Access Token：和后台里的 `OneBot Access Token` 保持一致
- 反向 HTTP 上报地址：
  - `http://127.0.0.1:8080/api/onebot?token=你后台填写的回调令牌`

如果程序和 NapCat 不在同一台机器：

- 把 `127.0.0.1` 改成程序所在机器的实际 IP

## 第四步：在后台添加推送对象

后台里分别添加：

- 群聊目标
- 私聊目标

建议：

- 常用群单独建一条目标
- 私聊管理员也建一条目标
- 先只启用 1 个群做测试

## 第五步：测试是否真的能收发

先在后台手动执行一次：

- 刷新数据
- 手动发送 `full` 或 `6m`

然后在群里试这些关键词：

- `传播`
- `6米`
- `太阳`
- `帮助`

如果能收到回复，说明：

- NapCat API 正常
- 反向 HTTP 正常
- 程序的目标配置正常

## 第六步：改成长期运行

推荐直接用仓库里的看门狗：

```bat
deploy\windows\run-watchdog.bat
```

它会做这些事：

- 拉起程序
- 程序退出后自动重启
- 把日志写到 `logs/`
- 不会反复弹浏览器

## 第七步：改成登录后自动启动

在程序目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File deploy\windows\install-startup-task.ps1 -AtLogon
```

如果你想开机就启动：

```powershell
powershell -ExecutionPolicy Bypass -File deploy\windows\install-startup-task.ps1 -AtStartup
```

## 推荐运行顺序

1. 先手动启动 `propagation_bot_single.exe` 或 `propagation_bot.exe`
2. 打开后台完成配置
3. 再启动 NapCat
4. 做一次手动发送测试
5. 最后启用看门狗和计划任务

## 常见建议

- 不要用主 QQ 号长期跑机器人
- 群发间隔尽量不要低于 `1000ms`
- 不要给太多群同时推送，建议逐步增加
- 先观察几天，再逐步缩短抓取频率
- 程序目录不要频繁移动，否则计划任务路径会失效

## 日志位置

- `logs/propagation-bot.stdout.log`
- `logs/propagation-bot.stderr.log`
- `logs/watchdog.log`
