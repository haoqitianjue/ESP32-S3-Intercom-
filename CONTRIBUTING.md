# 贡献指南 / Contributing Guide

[中文](#中文) | [English](#english)

---

<a name="中文"></a>
## 🇨🇳 中文

感谢你对 ESP32-S3-Intercom 项目的关注！我们欢迎任何形式的贡献。

### 🐛 报告问题

如果你发现了 Bug 或有功能建议，请通过 [GitHub Issues](https://github.com/haoqitianjue/ESP32-S3-Intercom-/issues) 提交。

**提交 Issue 时请包含：**

1. **问题描述** - 清晰描述问题或建议
2. **复现步骤** - 如何复现该问题
3. **预期行为** - 你期望发生什么
4. **实际行为** - 实际发生了什么
5. **环境信息**：
   - ESP32-S3 模块型号
   - PlatformIO 版本
   - 操作系统
6. **串口日志** - 如有相关错误信息

### 🔧 提交代码

#### 1. Fork 项目

点击页面右上角的 Fork 按钮，将项目复制到你的账户。

#### 2. 克隆到本地

```bash
git clone https://github.com/haoqitianjue/ESP32-S3-Intercom-.git
cd ESP32-S3-Intercom-
```

#### 3. 创建分支

```bash
git checkout -b feature/你的功能名称
# 或
git checkout -b fix/你的修复名称
```

#### 4. 进行修改

- 遵循现有代码风格
- 添加必要的注释
- 确保代码可以编译通过

#### 5. 提交更改

```bash
git add .
git commit -m "feat: 添加XXX功能"
# 或
git commit -m "fix: 修复XXX问题"
```

**Commit 消息规范：**

| 类型 | 说明 |
|------|------|
| `feat` | 新功能 |
| `fix` | Bug 修复 |
| `docs` | 文档更新 |
| `style` | 代码格式（不影响功能） |
| `refactor` | 代码重构 |
| `perf` | 性能优化 |
| `test` | 测试相关 |

#### 6. 推送并创建 PR

```bash
git push origin feature/你的功能名称
```

然后在 GitHub 上创建 Pull Request。

### 📝 代码规范

- **命名规范**：
  - 宏定义：全大写下划线分隔 `WIFI_CHANNEL`
  - 函数：驼峰命名 `initWiFi()`
  - 变量：驼峰命名 `currentChannel`

- **注释语言**：中文或英文均可

- **代码格式**：保持与现有代码风格一致

### 🌟 贡献方向

欢迎以下方面的贡献：

- 🐛 Bug 修复
- 📝 文档改进（特别是英文文档）
- 🔧 性能优化
- ✨ 新功能（请先提 Issue 讨论）
- 🌐 多语言翻译

### 📧 联系方式

- GitHub Issues: [提交问题](https://github.com/haoqitianjue/ESP32-S3-Intercom-/issues)
- Email: haoqitianjue@gmail.com

---

<a name="english"></a>
## 🇬🇧 English

Thank you for your interest in the ESP32-S3-Intercom project! We welcome contributions of all kinds.

### 🐛 Reporting Issues

If you find a bug or have a feature suggestion, please submit it via [GitHub Issues](https://github.com/haoqitianjue/ESP32-S3-Intercom-/issues).

**When submitting an issue, please include:**

1. **Description** - Clear description of the issue or suggestion
2. **Steps to reproduce** - How to reproduce the problem
3. **Expected behavior** - What you expected to happen
4. **Actual behavior** - What actually happened
5. **Environment**:
   - ESP32-S3 module model
   - PlatformIO version
   - Operating system
6. **Serial logs** - If relevant

### 🔧 Submitting Code

1. Fork the repository
2. Clone your fork locally
3. Create a new branch (`git checkout -b feature/your-feature`)
4. Make your changes
5. Commit with clear messages (`git commit -m "feat: add XXX feature"`)
6. Push to your fork
7. Create a Pull Request

### 📝 Code Style

- Follow existing code conventions
- Add comments where necessary
- Ensure code compiles without errors

### 📧 Contact

- GitHub Issues: [Submit Issue](https://github.com/haoqitianjue/ESP32-S3-Intercom-/issues)
- Email: haoqitianjue@gmail.com

---

**感谢你的贡献！ / Thank you for contributing!** 🙏

