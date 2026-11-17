# 被各阶段脚本 source：定位 MSVC 并导出 INCLUDE/LIB。
#
# 本仓库的 bash 只把 /mnt/<drive>/... 当真实路径，而 cl.exe / MSBuild 只认
# D:\... 写法，所以两套形式各留一个转换函数。

# D:\a\b → /mnt/d/a/b；本来就是 MSYS 路径则原样返回。
to_msys() {
    local p="${1//\\//}"
    case "$p" in
        [A-Za-z]:/*) printf '/mnt/%s%s' "$(printf '%s' "${p:0:1}" | tr 'A-Z' 'a-z')" "${p:2}" ;;
        *) printf '%s' "$p" ;;
    esac
}

# /mnt/d/a/b → D:\a\b；无法识别时原样返回。
to_win() {
    local p="$1"
    case "$p" in
        /mnt/[A-Za-z]/*)
            printf '%s:\\%s' \
                "$(printf '%s' "${p:5:1}" | tr 'a-z' 'A-Z')" \
                "$(printf '%s' "${p:7}" | tr '/' '\\')"
            ;;
        *) printf '%s' "$p" ;;
    esac
}

# 设置 QF_VS / QF_CL / QF_MSBUILD / QF_TOOLSET，并导出 INCLUDE 与 LIB。
qf_msvc_setup() {
    local vswhere="/mnt/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
    if [ ! -x "$vswhere" ]; then
        echo "错误：找不到 vswhere.exe（需要 Visual Studio 的 MSVC 工具集）" >&2
        return 1
    fi

    local vs_win
    vs_win="$("$vswhere" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
              -property installationPath | tr -d '\r' | head -1)"
    if [ -z "$vs_win" ]; then
        echo "错误：vswhere 没有找到带 C++ 工具集的 VS 实例" >&2
        return 1
    fi
    QF_VS="$(to_msys "$vs_win")"

    QF_CL="$(ls -1 "$QF_VS"/VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe 2>/dev/null | sort -V | tail -1)"
    if [ -z "$QF_CL" ]; then
        echo "错误：找不到 cl.exe（$QF_VS/VC/Tools/MSVC/*/bin/Hostx64/x64）" >&2
        return 1
    fi

    QF_MSBUILD="$("$vswhere" -latest -products '*' -requires Microsoft.Component.MSBuild \
                  -find 'MSBuild\**\Bin\MSBuild.exe' | tr -d '\r' | head -1)"
    if [ -n "$QF_MSBUILD" ]; then
        QF_MSBUILD="$(to_msys "$QF_MSBUILD")"
    fi

    # 新版 VS 的 PlatformToolset 号跟着 MSBuild\Microsoft\VC\vNN 目录走。
    local vcdir toolset
    vcdir="$(dirname "$QF_MSBUILD")/../../Microsoft/VC"
    toolset="$(ls -1 "$vcdir" 2>/dev/null | sort -V | tail -1 || true)"
    case "$toolset" in
        v180) QF_TOOLSET=v145 ;;
        v170) QF_TOOLSET=v143 ;;
        *)    QF_TOOLSET=v143 ;;
    esac

    # vcvars64.bat 才带 Windows SDK / UCrt 的 INCLUDE，cl 用它找 stdio.h 等系统头。
    # cmd /c 内联 call 会被引号咬到，落成临时 .bat 最稳。
    local cmd_exe="/mnt/c/Windows/system32/cmd.exe"
    [ -x "$cmd_exe" ] || cmd_exe="$(command -v cmd.exe || true)"
    if [ -z "$cmd_exe" ]; then
        echo "错误：找不到 cmd.exe" >&2
        return 1
    fi

    local vcvars_win bat dump tries
    vcvars_win="${vs_win//\//\\}\\VC\\Auxiliary\\Build\\vcvars64.bat"
    bat=".vcvars.$$.tmp.bat"
    printf '@echo off\r\ncall "%s" >nul 2>&1\r\nset\r\n' "$vcvars_win" > "$bat"
    dump=""
    for tries in 1 2 3; do
        dump="$(MSYS_NO_PATHCONV=1 "$cmd_exe" /c "$bat" 2>/dev/null || true)"
        case "$dump" in *INCLUDE=*) break ;; esac
        dump=""
    done
    rm -f "$bat"

    local include_win lib_win
    include_win="$(printf '%s\n' "$dump" | tr -d '\r' | sed -n 's/^INCLUDE=//p' | head -1)"
    lib_win="$(printf '%s\n' "$dump" | tr -d '\r' | sed -n 's/^LIB=//p' | head -1)"
    if [ -z "$include_win" ]; then
        echo "错误：无法从 vcvars64.bat 取得 INCLUDE" >&2
        return 1
    fi
    export INCLUDE="$include_win"
    export LIB="$lib_win"
    return 0
}
