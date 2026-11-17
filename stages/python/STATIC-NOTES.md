# Python 阶段：静态 CPython 构建记录

产物：`stages/python/out/python311.lib`（静态库）+ `stages/python/out/qf_py.obj`。
**没有使用 DLL 导入库**：`out/python311.lib` 是从 CPython 源码编译出的静态库
（32,961,982 字节，`nm` 里 2143 个已定义代码符号，含 `Py_Initialize`、
`Py_InitializeFromConfig`、`Py_FinalizeEx`、`PyObject_Call` 等全部公开 C API），
不是 `D:/Python/Python311/libs/python311.lib` 那种导入库。

## 复现

```bash
bash stages/python/build_static_python.sh   # 下载 + 生成 frozen 模块 + 构建静态库
bash stages/python/build.sh                 # cl 编译 qf_py.c，并拷贝静态库到 out/
bash stages/python/test.sh                  # MSVC 链接自测，最后打印 python stage OK
```

版本：CPython **3.11.9**（`stages/python/vendor/Python-3.11.9`，源码来自
`https://www.python.org/ftp/python/3.11.9/Python-3.11.9.tgz`）。
工具链：VS 2026（MSVC 14.51、PlatformToolset=v145）、MSBuild `Current/bin`。

## 实际步骤

1. 解压源码到 `stages/python/vendor/`。
2. `PCbuild/get_externals.bat --no-tkinter --no-openssl --no-libffi`（失败细节见下）。
3. `msbuild PCbuild/_freeze_module.vcxproj`：该工程是自足的（`Programs/_freeze_module.c`
   + `PC/config_minimal.c` + 核心源），其 AfterBuild 规则会生成
   `Python/frozen_modules/*.h` 与 `Python/deepfreeze/deepfreeze.c`。
4. `msbuild PCbuild/pythoncore.vcxproj`（ConfigurationType 已改为 StaticLibrary）。

## 踩坑与处理（都在 vendor 目录内改，脚本可重放）

1. **`get_externals.bat` 证书链失败**：`PCbuild/get_external.py` 用 `urlretrieve`，
   本机报 `[SSL: CERTIFICATE_VERIFY_FAILED] unable to get issuer certificate`。
   `-f` 不是这个脚本的选项（那是 `build.bat` 的）。回落到 `curl` 抓同一个
   `python/cpython-source-deps` 的 `zlib-1.3.1.zip`。pythoncore 只依赖 zlib；
   openssl/bzip2/sqlite/xz 属于扩展模块，静态库不需要。
2. **发布 tarball 不含 frozen 产物**：`Python/frozen_modules/` 只有 README，
   `Python/frozen.c` 却已经按 deepfreeze 模式引用了 `_Py_get_<mod>_toplevel`。
   必须先建 `_freeze_module` 工程生成头文件与 `deepfreeze.c`，否则 pythoncore 链接不过。
   `_freeze_module` 的 AfterBuild 用 `$(PythonForBuild)` 跑 `Tools/scripts/deepfreeze.py`，
   本脚本显式传入本机 Python 3.11（字节码版本必须与 3.11 一致）。
3. **默认配置是 DLL**：`pythoncore.vcxproj` 的 `ConfigurationType=DynamicLibrary`，
   宏里有 `_USRDLL;Py_ENABLE_SHARED`。改成 `StaticLibrary`，宏换成
   `Py_NO_ENABLE_SHARED`（`PC/pyconfig.h` 只要没看到 `Py_NO_ENABLE_SHARED`
   就会自己定义 `Py_ENABLE_SHARED`，所以必须“定义出来”，不能只是“不定义”）。
4. **LTCG 产物 binutils 不认**：`pyproject.props` 默认
   `WholeProgramOptimization=true`（`/GL`），产出匿名 IL 对象，
   `nm`/`ld` 报 `file format not recognized`。改成 `false` 后对象是普通 COFF
   （`machine 0x8664`），MinGW 主干才能消费。
5. **`PyWin_DLLhModule` 未定义（LNK2001）**：`PC/dl_nt.c` 把定义和 `DllMain`
   整个包在 `#ifdef Py_ENABLE_SHARED` 里，而 `Modules/getpath.c`、
   `Python/dynload_win.c`、`Python/sysmodule.c` 照旧引用它。补一个
   `PC/qf_static_support.c`（`HMODULE PyWin_DLLhModule = NULL;`）并加进工程。
6. **静态库没有编译期 home**：`PREFIX/EXEC_PREFIX` 都是 `NULL`，`getpath.py`
   会打印 `Could not find platform independent libraries <prefix>`；在 Windows 上
   它随后还会去读注册表里已安装的 CPython，把开发机路径塞进 `sys.path`
   （本机实测会落到 `D:\Python\Python311\Lib`）。因此 `qf_py.c` 显式把
   `PYTHONHOME` 写进 `PyConfig.home`。集成时把 `PYTHONHOME` 指向载荷解包目录
   （含 `Lib/`）即可，路径不再依赖开发机。
7. **本仓库脚本环境不透传环境变量**：从脚本里执行 native exe 时
   `PYTHONHOME=... ./x.exe` 不生效（实测子进程拿不到该变量），所以 `test.sh` 用
   `-DQF_TEST_PYTHONHOME="..."` 把标准库目录编进自测程序，由 `_putenv_s` 落进进程环境。
   真实运行（用户 shell / 集成方设置的环境）不受影响。
8. **MSBuild 会顺手调 git**：`_GetBuildInfo` 目标会执行 `git name-rev/rev-parse/describe`。
   构建脚本传 `/p:GIT=__no_git__`，让这些命令直接失败（ContinueOnError），
   只多出 3 条 `MSB3073` 警告，仓库的 git 元数据不被触碰；`sys._git` 为空字符串。
9. 其余噪声：`Include/cpython/pytime.h` 的 `C4115 timeval` 警告来自 CPython 自身；
   `Toolset v145 is not used for official builds` 是上游的提示性警告；
   静态库里含 `python_nt.res` 资源成员，MSVC 链接无碍。

## 跨 CRT 链接（留给集成）

`qf_py.obj` 与 `python311.lib` 都用 MSVC 默认的 `/MD`（UCrt + vcruntime140 导入库），
二者 CRT 一致，用 `cl` 链接已经验证可运行。改成 MinGW 主干后需要：

- 链接 `ucrtbase`/`msvcrt` 系的导入库，或在兼容 stub 里补齐；
- `qf_py.obj` 的未定义符号（`nm -u`）：
  `__security_check_cookie`、`__security_cookie`、`__GSHandlerCheck`、
  `__report_rangecheckfailure`、`_fltused`、`__imp_{malloc,free,fopen,fclose,getenv,
  strerror,_errno,__stdio_common_vsprintf,_wassert}`、`memcpy`、`strlen`，
  其余都是 `Py_*`/`_Py_*` API。除 Python API 外都属于 ADR-0003 里那个 CRT 兼容 stub 的职责，
  其中 `__report_rangecheckfailure` 目前 ADR 没列，若 MinGW 链接报缺需要补一个空实现。
- `python311.lib` 里的 `python_nt.res` 与 `dl_nt.obj`（空壳）不会被强引用。
- `qf_py.obj` 的 `.drectve` 是 MSVC 自动塞进去的
  `/DEFAULTLIB:"MSVCRT" /DEFAULTLIB:"OLDNAMES"` 和
  `/alternatename:_Avx2WmemEnabled=_Avx2WmemEnabledWeakValue`。
  MinGW 侧若对这两条 `DEFAULTLIB` 报找不到库，忽略或补 `-lmsvcrt` 即可；
  `alternatename` 指到的是 UCrt 内部的弱符号，不影响我们自己的符号。

## 其它

- `stages/python/vendor/` 是构建输入（源码 + 生成的 frozen 头 + 中间产物），
  目前不在 `.gitignore` 里；是否入库由集成方决定（源码树约 150 MB，
  其中 `PCbuild/amd64`、`PCbuild/obj` 属于可重生成的中间产物）。
- 测试只覆盖阶段自身：加载插件、四种契约类型往返、`import` 标准库、
  未知函数与插件异常都返回 `QF_ERR_UDF` 并带中文前缀诊断。
