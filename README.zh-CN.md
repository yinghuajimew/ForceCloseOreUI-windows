<div align="center">

<h1 align="center">ForceCloseOreUI — Windows</h1>

<p>强制关闭 Minecraft 基岩版（Windows）的 OreUI，恢复经典 JSON-UI。</p>

<p>
<a href="./README.md">English</a> · <b>中文</b>
</p>

</div>

<br/>

## 这是什么

一个 x64 Windows DLL，通过 Microsoft Detours 钩住 Minecraft 基岩版初始化 OreUI 配置的函数，
把 `OreUIConfig` 的每一项强制设为 `false`，游戏于是回退到 JSON-UI。

它在运行时挂钩、不改文件，所以同一个 DLL 能跨多个游戏版本使用——只要特征码能命中。

## 运行要求

- Minecraft 基岩版 **Windows** 版，x64
- 需要把 DLL 加载进游戏进程的手段——**这通常是卡住大家的一步**

## ⚠️ 你需要一个注入器

**光有 DLL 没用。** Windows 版 Minecraft 运行在 UWP / AppContainer 沙箱里，
把 DLL 放进某个文件夹并不会让它被加载。必须有东西把它注入到 `Minecraft.Windows.exe`。

有两个实际可行的办法。

### 方案 A —— 用支持 Mod 的启动器（推荐）

最省事，因为启动器会替你处理 UWP 限制：

**[QYCottage/mc-w10-version-launcher](https://github.com/QYCottage/mc-w10-version-launcher)**

这个启动器让 Minecraft 运行在 **Windows 应用沙箱之外**，并从普通 `mods` 文件夹加载 DLL，
还能让多个游戏版本并存。

1. 安装启动器（需要管理员权限，并开启 Windows 开发者模式）
2. 用它下载一个 Minecraft 版本
3. 把 `ForceCloseOreUI.dll` 放进该版本的 `mods` 文件夹
4. 启动游戏

### 方案 B —— 用独立注入器

如果你用别的方式运行 Minecraft，可以在游戏启动后把 DLL 注入 `Minecraft.Windows.exe`：

- **[ambiennt/MCBE-DLL-Injector](https://github.com/ambiennt/MCBE-DLL-Injector)** ——
  一个小巧的开源注入器，默认目标就是 Minecraft 基岩版，并且会自动给 DLL 加上
  UWP 应用所需的 *All Application Packages* 权限。
  如果报缺少 `VCRUNTIME140_1.dll`，装一下
  [x64 VC++ 运行库](https://support.microsoft.com/en-us/help/2977003)。

请在**主菜单出现之后**再注入。在启动过程中注入会和游戏自身的初始化抢时序。

> [!NOTE]
> 杀毒软件经常把 DLL 注入器报成可疑程序——那是注入手法本身触发的，和本项目无关。
> 源码是公开的，运行前请自己看一眼。

## 配置在哪里

```
%APPDATA%\Minecraft Bedrock\mods\ForceCloseOreUI\
├── config.json        # 要关掉哪些项，首次挂钩成功后生成
├── signatures.json    # 记下能用的特征码与 detour 变体（自动维护）
├── debug.log          # 会话日志
└── crash_native.log   # 原生崩溃现场，崩了才有
```

`%APPDATA%` 不可用时会回退到游戏工作目录下的 `mods\ForceCloseOreUI\`。
如果装的是沙箱版（微软商店版），同样的文件也会出现在：

```
C:\Users\[用户名]\AppData\Local\Packages\Microsoft.MinecraftUWP_8wekyb3d8bbwe\AC\mods\ForceCloseOreUI
```

## 这个版本改了什么

本分支对上游 DLL 做了较大重写：现在是**单文件**（不再依赖那些辅助头文件），
挂钩流程也重做了。

### 1. 修复：所有钩子都报「not called」然后被丢弃

旧流程装上钩子后固定等 **10 秒**，目标函数没被调用就 detach、换下一个 detour 变体。
但 OreUI 配置初始化函数往往只在玩家进入某个界面时才调用，那时距启动可能早已超过十秒。
结果正确的钩子被销毁，看起来就像「没有任何特征码匹配」。

现在拆成两段：

- 先等 **2.5 秒**，覆盖「很快就调用」的常见情况
- 再进入**观察期**，最长 **30 秒**，等到的是确认，不是删除

只有确实证明钩错了才会拆。整轮扫描由 **5 分钟** 总预算兜底。

### 2. 修复：配置表为空被当成挂钩失败

如果目标函数执行了、但 `mConfigs` 仍是空的，旧代码会判定「没被调用」并 detach。
现在算作**有效**挂钩——函数确实被调用到了，只是配置还没填充，后续调用会生效。
只有配置表明显畸形（`> 2000` 项）才算地址错了。

### 3. 扫描提速——锚点预筛

原实现每个偏移都要比对整条模式。现在先找出模式里**最长的一段连续固定字节**作为锚点，
绝大多数位置一次比较就被排除。

### 4. 特征码更多

内置兜底列表从 1 条扩到 **9 条**，大致覆盖 2025-10 到 1.26.60：

| 特征码 | 覆盖 |
|---|---|
| 放宽版（栈偏移通配） | 1.26.40 ~ 1.26.50 |
| 1.26.60 | 与放宽版仅差结尾 4 字节（`45 89 C6`） |
| 1.26.50 | 精确 |
| 1.26.20 / 1.26.30 | 精确 |
| 5 条早期版本 | 2025-10 ~ 2026-03 |

其中 1.26.60 的特征码取自上游提交 `1fd8606`。

## 编译

```sh
xmake f -p windows -m release -y
xmake
# -> build/windows/x64/release/ForceCloseOreUI.dll
```

需要 Visual Studio（MSVC）与 xmake。CI（`.github/workflows/build.yml`）在
`windows-latest` 上构建并上传 DLL。

## 致谢

基于 QYCottage 及贡献者的原项目 [ForceCloseOreUI](https://github.com/QYCottage/forceCloseOreUI)。
