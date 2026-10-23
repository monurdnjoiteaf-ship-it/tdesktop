# Telegram Desktop 定制版开发

本分支用于开发 Windows x64 Telegram Desktop 定制版。

## 目标
- 保留 Telegram Desktop 的登录、聊天、消息等原有功能。
- 支持将 MP3/OGG 音频作为 Telegram 语音消息发送。
- 增加机器绑定的离线签名授权、有效期和到期检查。
- 单独提供 KeyGen 命令行工具。
- 通过 GitHub Actions 编译并上传 Windows 产物。

## 状态
- [x] 创建独立开发分支
- [ ] 修改客户端语音发送代码
- [ ] 集成客户端许可证验证
- [ ] 编写并编译 KeyGen
- [ ] Windows x64 构建通过
- [ ] 测试并发布可下载产物

## 安全原则
授权文件使用非对称数字签名验证；私钥不得写入公开仓库或客户端程序。公开源码不应包含真实用户的 Telegram 会话文件、token 或密钥。

## 构建进度
请查看仓库 Actions 页面的实际运行状态。仅有文件提交不代表 EXE 已编译成功。
