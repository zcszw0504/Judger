#!/bin/bash
#
# 编译脚本
#
# 用法：$0 <compile_script> <chrootdir> <workdir> <source file...>
#
# <compile_script>  编译的脚本
# <chrootdir>       chroot directory
# <workdir>         本次评测的工作文件夹，比如 /tmp/judger0/judging_5322222/
#                   编译生成的可执行文件在该文件夹中，编译器输出也在该文件夹中
# <source file...>  需要参与编译的源文件
#
# 评测系统通过调用不同的编译脚本来实现多语言支持。
#
# 编译脚本的调用参数格式为：
#
#   <compile_script> <dest> <source file...>
#
#   <dest>     可执行文件的文件名，如 main, main.jar
#   <source file...>
#
# 环境变量：
#   $RUNGUARD
#   $RUNUSER
#   $RUNGROUP
#   $SCRIPTMEMLIMIT 编译脚本运行内存限制
#   $SCRIPTTIMELIMIT 编译脚本执行时间
#   $SCRIPTFILELIMIT 编译脚本输出限制
#   $E_COMPILER_ERROR 编译失败返回码
#   $E_INTERNAL_ERROR 内部错误返回码

set -e
trap error EXIT

cleanexit ()
{
    trap - EXIT

    #chmod go= "$WORKDIR/compile"
    logmsg $LOG_DEBUG "exiting, code = '$1'"
    exit $1
}

. "$JUDGE_UTILS/utils.sh" # runcheck
. "$JUDGE_UTILS/logging.sh" # logmsg, error
. "$JUDGE_UTILS/chroot_setup.sh" # chroot_setup
. "$JUDGE_UTILS/runguard.sh" # read_metadata

CPUSET=""
CPUSET_OPT=""
OPTIND=1
while getopts "n:" opt; do
    case $opt in
        n)
            CPUSET="$OPTARG"
            ;;
        :)
            >&2 echo "Option -$OPTARG requires an argument."
            ;;
    esac
done

shift $((OPTIND-1))
[ "$1" == "--" ] && shift

if [ -n "$CPUSET" ]; then
    CPUSET_OPT="--cpuset $CPUSET"
fi

LOGLEVEL=$LOG_DEBUG
PROGNAME="$(basename "$0")"

if [ "$DEBUG" ]; then
    export VERBOSE=$LOG_DEBUG
    logmsg $LOG_NOTICE "debugging enabled, DEBUG='$DEBUG'"
else
    export VERBOSE=$LOG_ERR
fi

[ $# -ge 4 ] || error "Not enough arguments."
COMPILE_SCRIPT="$1"; shift
CHROOTDIR="$1"; shift
WORKDIR="$1"; shift

if [ ! -d "$WORKDIR" ] || [ ! -w "$WORKDIR" ] || [ ! -x "$WORKDIR" ]; then
    error "Work directory is not found or not writable: $WORKDIR"
fi

[ -x "$COMPILE_SCRIPT" ] || error "Compile script not found or not executable: $COMPILE_SCRIPT"
[ -x "$RUNGUARD" ] || error "runguard not found or not executable: $RUNGUARD"

cd "$WORKDIR"
RUNDIR="$WORKDIR/run-compile"

[ -d "$RUNDIR/merged/judge" ] && force_umount "$RUNDIR/merged/judge" || /bin/true
[ -d "$RUNDIR/merged/compile" ] && force_umount "$RUNDIR/merged/compile" || /bin/true
[ -d "$RUNDIR/merged" ] && chroot_stop "$CHROOTDIR" "$RUNDIR/merged"
[ -d "$RUNDIR/merged" ] && force_umount "$RUNDIR/merged" || /bin/true

mkdir -p "$WORKDIR/compile"
$GAINROOT chmod -R 777 "$WORKDIR/compile"
$GAINROOT chown -R "$RUNUSER" "$WORKDIR/compile"
cd "$WORKDIR/compile"
touch compile.meta

exec >>compile.out
exec 2>&1


for src in "$@"; do
    [ -r "$WORKDIR/compile/$src" ] || error "source file not found: $src"
done

if [ ! -z "$ENTRY_POINT" ]; then
    ENVIRONMENT_VARS="-V ENTRY_POINT=$ENTRY_POINT"
fi

mkdir -p "$RUNDIR"; chmod 777 "$RUNDIR"

chmod -R +x "$COMPILE_SCRIPT"

mkdir -p "$RUNDIR/work"; chmod 777 "$RUNDIR/work"
mkdir -p "$RUNDIR/work/judge"; chmod 777 "$RUNDIR/work/judge"
mkdir -p "$RUNDIR/work/compile"; chmod 777 "$RUNDIR/work/compile"
mkdir -p "$RUNDIR/merged"; chmod 777 "$RUNDIR/merged"
mkdir -p "$RUNDIR/ofs"; chmod 777 "$RUNDIR/ofs"
$GAINROOT mount -t overlay overlay -olowerdir="$CHROOTDIR",upperdir="$RUNDIR/work",workdir="$RUNDIR/ofs" "$RUNDIR/merged"
$GAINROOT mount --bind "$WORKDIR/compile" "$RUNDIR/merged/judge"
$GAINROOT mount --bind -o ro "$COMPILE_SCRIPT" "$RUNDIR/merged/compile"

chroot_start "$CHROOTDIR" "$RUNDIR/merged"

# 调用 runguard 来执行编译命令
runcheck $GAINROOT "$RUNGUARD" ${DEBUG:+-v} $CPUSET_OPT -c \
        --root "$RUNDIR/merged" \
        --work /judge \
        --user "$RUNUSER" \
        --group "$RUNGROUP" \
        --memory-limit "$SCRIPTMEMLIMIT" \
        --cpu-time "$SCRIPTTIMELIMIT" \
        --standard-output-file compile.tmp \
        --standard-error-file compile.tmp \
        --out-meta compile.meta \
        $ENVIRONMENT_VARS -- \
        "/compile/run" run "$@"

chroot_stop "$CHROOTDIR" "$RUNDIR/merged"

# 删除挂载点，因为我们已经确保有用的数据在 $WORKDIR/compile 中，因此删除挂载点即可。
force_umount "$RUNDIR/merged/judge"
force_umount "$RUNDIR/merged/compile"
force_umount "$RUNDIR/merged"
rm -rf "$RUNDIR"

$GAINROOT chown -R "$(id -un):" "$WORKDIR/compile"
chmod -R go-w+x "$WORKDIR/compile"

# 检查是否编译器出错/runguard 崩溃
if [ ! -s compile.meta ]; then
    echo "Runguard exited with code $exitcode and 'compile.meta' is empty, it likely crashed."
    cat compile.tmp
    cleanexit ${E_INTERNAL_ERROR:--1}
fi

if grep -E '^internal-error: .+$' compile.meta >/dev/null 2>&1; then
    echo "Internal Error"
    cat compile.tmp
    cleanexit ${E_INTERNAL_ERROR:-1}
fi

# Check if the compile script auto-detected the entry point, and if
# so, store it in the compile.meta for later reuse, e.g. in a replay.
grep '[Dd]etected entry_point: ' compile.tmp | sed 's/^.*etected //' >>compile.meta

echo "checking compilation exit-status"
read_metadata compile.meta

# 检查是否编译超时，time-result 可能为空、soft-timelimit、hard-timelimit，空表示没有超时
if grep '^time-result: .*timelimit' compile.meta >/dev/null 2>&1; then
    echo "Compilation aborted after $SCRIPTTIMELIMIT seconds."
    cat compile.tmp
    cleanexit ${E_COMPILER_ERROR:--1}
fi

if [ $progexit -ne 0 ]; then
    echo "Compilation failed with exitcode $progexit."
    cat compile.tmp
    cleanexit ${E_COMPILER_ERROR:--1}
fi

# 检查是否成功编译出程序
if [ ! -f run ] || [ ! -x run ]; then
    echo "Compilation failed: executable is not created."
    cat compile.tmp
    cleanexit ${E_COMPILER_ERROR:--1}
fi

cat compile.tmp
cleanexit 0
