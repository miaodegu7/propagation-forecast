# 业余无线电传播助手

这是一个面向业余无线电台站的常驻服务，核心使用 C 编写，适合部署在服务器上长期运行。它会抓取当天传播相关数据，做中文分析，并通过 OneBot 11 / NapCat 向 QQ 群聊和私聊推送。

当前版本已经包含这些能力：

- `HAMqsl` 全量太阳 / HF / VHF 数据抓取
- `PSKReporter MQTT` 6m 实时 spot 监控
- `Open-Meteo` 本地天气与日照辅助判断
- `F5LEN Asia Tropo` 对流层 6m 辅助分析
- `IMO` 最近主要流星雨倒计时
- 可选 `N2YO radiopasses` 卫星过境推荐
- 地磁风暴分级告警
- 6m 一般提醒 / 重点提醒 / 强提醒
- 可配置问词、模板、发送频率、时区、网格、目标、卫星筛选
- 中文网页管理后台

## 运行方式

程序首次启动会自动创建 SQLite 数据库并写入默认配置：

```bash
./propagation_bot ./propagation.db
```

默认后台地址：

- `http://0.0.0.0:8080/`

默认 OneBot 回调路径：

- `/api/onebot?token=你的回调令牌`

## 后台可配置项

后台现在支持修改这些核心内容：

- 台站呼号、主网格、PSK 监控网格、经纬度、海拔、时区
- OneBot API 地址、Access Token、回调令牌、机器人名称 / QQ / 密码保存项
- HAMqsl / 天气 / Tropo / 流星雨 / 卫星 / PSK 评估的独立抓取频率
- 每分钟问答限流
- 地磁告警阈值
- 6m 告警间隔与 PSK 触发条数
- 是否附带来源网址
- 是否发送 HAMqsl 小组件图 / F5LEN 图
- HAMqsl 显示字段筛选
- 多条定时发送规则
- 推送目标增删改停用
- 卫星白名单与线性 / 非线性筛选
- 完整简报 / 6m 简报 / 太阳简报 / 地磁告警 / 6m 告警 / 帮助回复模板
- 用户可自定义问词

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

## NapCat / OneBot 11 建议

- HTTP API：`http://127.0.0.1:3000`
- 反向 HTTP 上报：`http://你的服务器:8080/api/onebot?token=你在后台设置的令牌`
- Access Token：与后台保持一致

机器人会主动发送：

- 定时日报
- 地磁升级告警
- 6m 分级提醒

机器人也会被动回复：

- 传播
- 6m / 6米
- 太阳 / 磁暴 / 空间天气
- 帮助

这些问词都可以在后台改。

## 数据说明

- `HAMqsl` 负责太阳活动、HF 段况、VHF 条件、小组件图
- `PSKReporter` 网页接口常受 Cloudflare 影响，所以程序改用稳定的 `MQTT`
- `F5LEN Asia` 提供亚洲对流层热力图，程序会按你的台站位置取样
- `Open-Meteo` 只作为 6m 的辅助参考，不替代实测 spot
- `IMO` 默认用于流星雨倒计时
- `N2YO` 需要你自己填写 API Key 才能启用卫星过境推荐

## 长时间运行

程序在设计上做了这些处理：

- SQLite 使用 `WAL`
- 各数据源独立轮询
- 手动查询不会重置抓取周期
- MQTT 自动重连
- 后台和机器人共用同一份快照缓存
- 适合配合 `systemd` 或其他守护方式常驻

仓库内已提供 `systemd` 示例：

- `deploy/propagation-bot.service.example`

## EXE 打包说明

我已经把代码结构整理到适合后续打包的状态，但当前这个环境没有可用的 `gcc/clang` 以及对应的静态依赖链，所以这次提交里还没有产出真正的单文件 `EXE`。如果你后面提供 Windows 的构建工具链，或者我继续给你补自动化构建流程，就可以继续往单文件打包推进。
