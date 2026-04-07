# Windows 常驻运行

这套脚本用于把 `propagation_bot.exe` 作为长期常驻程序运行，适合配合 `NapCat / OneBot 11` 使用。

## 文件说明

- `run-watchdog.bat`
  - 直接启动看门狗
- `watchdog.ps1`
  - 负责拉起 `propagation_bot.exe`
  - 程序退出后自动等待几秒再重启
  - 自动创建 `runtime/` 和 `logs/`
- `install-startup-task.ps1`
  - 注册 Windows 计划任务
  - 可设置为登录后自启，或开机自启

## 推荐方式

1. 把程序目录放到固定位置，不要频繁移动。
2. 先在后台页面配好：
   - `OneBot API 地址`
   - `OneBot Access Token`
   - `回调令牌`
   - `群发间隔(ms)`
   - `失败重试次数`
   - `重试等待(ms)`
3. 确认 NapCat 已开启：
   - HTTP API
   - 反向 HTTP 上报
4. 手动运行一次：
   - `deploy\windows\run-watchdog.bat`
5. 没问题后再注册自启：
   - `powershell -ExecutionPolicy Bypass -File deploy\windows\install-startup-task.ps1 -AtLogon`

## 日志位置

- `logs/propagation-bot.stdout.log`
- `logs/propagation-bot.stderr.log`
- `logs/watchdog.log`

## 风控建议

- 使用专门的 QQ 号，不要拿主号做高频推送。
- 群发间隔建议保持在 `1000ms` 以上。
- `6m` 和地磁告警已经按升级和间隔控制，不建议再额外缩短太多。
- 回复关键词限流建议保留，不要关闭。
