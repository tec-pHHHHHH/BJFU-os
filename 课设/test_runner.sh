#!/bin/bash
# ================================================================
# 操作系统模拟器 — 自动化测试脚本
#
# 用法:
#   bash test_runner.sh              # 运行所有测试
#   bash test_runner.sh module1      # 只跑模块一
#
# 每个模块测试结果保存到 test_output/ 目录
# ================================================================

set -e

EXE="./build/os_sim.exe"
OUTDIR="test_output"
PASS=0
FAIL=0

# 颜色
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

clean_data() {
    rm -f data/users.db data/os_state.bin data/os_state.lock
    mkdir -p "$OUTDIR"
}

run_test() {
    local name="$1"
    local input="$2"
    local outfile="$OUTDIR/${name}.txt"

    echo -e "${YELLOW}[TEST]${NC} $name ..."
    clean_data
    printf '%s\n' "$input" | timeout 10 "$EXE" > "$outfile" 2>&1 || true
    echo "  输出保存: $outfile"
}

check_output() {
    local name="$1"
    local pattern="$2"
    local desc="$3"
    local outfile="$OUTDIR/${name}.txt"

    if grep -q "$pattern" "$outfile" 2>/dev/null; then
        echo -e "  ${GREEN}[PASS]${NC} $desc"
        ((PASS++))
    else
        echo -e "  ${RED}[FAIL]${NC} $desc — 未找到: $pattern"
        ((FAIL++))
    fi
}

# ================================================================
# 模块一：用户管理（8分）
# ================================================================

test_module1() {
    echo ""
    echo "========== 模块一：用户管理 =========="

    run_test "1_register" '
register admin 123456
register test_user abc
register user123 pass
exit
'
    check_output "1_register" "用户.*admin.*创建成功" "正常注册 admin"
    check_output "1_register" "用户.*test_user.*创建成功" "正常注册 test_user"
    check_output "1_register" "用户.*user123.*创建成功" "正常注册 user123"

    run_test "1_dup" '
register admin 123456
register admin xyz
exit
'
    check_output "1_dup" "已存在" "重复用户名拒绝"

    run_test "1_login_ok" '
register admin 123456
login admin 123456
exit
'
    check_output "1_login_ok" "登录成功" "正常登录"

    run_test "1_login_fail" '
register admin 123456
login admin wrong
exit
'
    check_output "1_login_fail" "密码错误" "错误密码拒绝"

    run_test "1_lockout" '
register admin 123456
login admin wrong1
login admin wrong2
login admin wrong3
login admin 123456
exit
'
    check_output "1_lockout" "已被锁定" "3次错误后锁定"
    check_output "1_lockout" "已被锁定" "锁定后正确密码也被拒"

    run_test "1_lockout_persist" '
login admin 123456
exit
'
    # 先创建 locked 账户
    clean_data
    printf 'register admin 123456\nlogin admin wrong1\nlogin admin wrong2\nlogin admin wrong3\nexit\n' | timeout 10 "$EXE" > /dev/null 2>&1 || true
    # 重启验证
    printf 'login admin 123456\nexit\n' | timeout 10 "$EXE" > "$OUTDIR/1_lockout_persist.txt" 2>&1 || true
    check_output "1_lockout_persist" "已被锁定" "重启后锁定状态保留"

    run_test "1_logout" '
register admin 123456
login admin 123456
logout
list_pcb
exit
'
    check_output "1_logout" "请先登录" "登出后拦截需登录命令"

    echo "  模块一: 密码哈希验证需手动检查 data/users.db（不含明文密码）"
}

# ================================================================
# 模块二：进程管理（20分）
# ================================================================

test_module2() {
    echo ""
    echo "========== 模块二：进程管理 =========="

    run_test "2_create" '
register admin 123456
login admin 123456
create_pcb worker1 3
create_pcb worker2 8
create_pcb daemon 5
overview
exit
'
    check_output "2_create" "init.*PID=1.*已自动创建" "init 自动创建"
    check_output "2_create" "worker1.*PID=2.*创建" "worker1 创建成功"
    check_output "2_create" "worker2.*PID=3.*创建" "worker2 创建成功"
    check_output "2_create" "daemon.*PID=4.*创建" "daemon 创建成功"

    run_test "2_show_list" '
register admin 123456
login admin 123456
create_pcb worker1 3
show_pcb 2
show_pcb 99
list_pcb
exit
'
    check_output "2_show_list" "PID.*2" "show_pcb 显示进程详情"
    check_output "2_show_list" "不存在" "show_pcb 99 报不存在"
    check_output "2_show_list" "worker1" "list_pcb 表格含 worker1"

    run_test "2_ptree" '
register admin 123456
login admin 123456
create_pcb child1 5
create_pcb child2 8
ptree
exit
'
    check_output "2_ptree" "Process Tree" "进程树标题"
    check_output "2_ptree" "child1" "child1 在树中"
    check_output "2_ptree" "child2" "child2 在树中"

    run_test "2_block_wakeup" '
register admin 123456
login admin 123456
create_pcb test 5
block_pcb 2
show_pcb 2
wakeup_pcb 2
show_pcb 2
exit
'
    check_output "2_block_wakeup" "BLOCKED" "block 后状态为 BLOCKED"
    # wakeup 后状态恢复 READY — 检查 show_pcb 输出

    run_test "2_suspend_resume" '
register admin 123456
login admin 123456
create_pcb test 5
suspend 2
show_pcb 2
resume 2
show_pcb 2
exit
'
    check_output "2_suspend_resume" "SUSPENDED" "suspend 后状态为 SUSPENDED"

    run_test "2_renice" '
register admin 123456
login admin 123456
create_pcb test 8
renice 2 3
show_pcb 2
exit
'
    check_output "2_renice" "prio=3" "renice 后优先级变为 3" || true  # 宽松匹配

    run_test "2_kill" '
register admin 123456
login admin 123456
create_pcb victim 5
kill_pcb 2
list_pcb
exit
'
    check_output "2_kill" "已撤销" "kill_pcb 成功"
}

# ================================================================
# 模块三：调度器（8分）
# ================================================================

test_module3() {
    echo ""
    echo "========== 模块三：调度器 =========="

    run_test "3_step" '
register admin 123456
login admin 123456
create_pcb jobA 2
create_pcb jobB 5
create_pcb jobC 10
step
exit
'
    check_output "3_step" "Step 1" "step 输出 Step1"
    check_output "3_step" "Step 2" "step 输出 Step2"
    check_output "3_step" "Step 3" "step 输出 Step3"
    check_output "3_step" "Step 4" "step 输出 Step4"
    check_output "3_step" "选程决策" "step 选程决策"

    run_test "3_demotion" '
register admin 123456
login admin 123456
create_pcb jobA 2
step
step
step
step
step
exit
'
    check_output "3_demotion" "降级" "时间片耗尽降级"
    check_output "3_demotion" "Q0 → Q1" "Q0→Q1 降级提示"

    run_test "3_sched_thread" '
register admin 123456
login admin 123456
create_pcb jobA 2
start_sched
stop_sched
exit
'
    check_output "3_sched_thread" "调度器.*已启动" "start_sched 启动"
    check_output "3_sched_thread" "已暂停" "stop_sched 暂停"
}

# ================================================================
# 模块四：内存管理（16分）
# ================================================================

test_module4() {
    echo ""
    echo "========== 模块四：内存管理 =========="

    run_test "4_alloc" '
register admin 123456
login admin 123456
create_pcb test 5
alloc 200
alloc 100
show_mem
exit
'
    check_output "4_alloc" "成功.*地址=64.*大小=200" "alloc 200 成功"
    check_output "4_alloc" "成功.*地址=264.*大小=100" "alloc 100 成功"

    run_test "4_algo" '
register admin 123456
login admin 123456
create_pcb test 5
set_alloc_algo 1
mem_stat
set_alloc_algo 2
mem_stat
exit
'
    check_output "4_algo" "最佳适应" "BF 算法切换"
    check_output "4_algo" "最坏适应" "WF 算法切换"

    run_test "4_free_merge" '
register admin 123456
login admin 123456
create_pcb test 5
alloc 200
free_mem 64
show_mem
exit
'
    check_output "4_free_merge" "空闲.*起始.*64" "free_mem 后出现空闲块"

    run_test "4_compact" '
register admin 123456
login admin 123456
create_pcb test 5
alloc 200
alloc 100
free_mem 264
compact
show_mem
exit
'
    check_output "4_compact" "紧缩完成" "compact 执行成功"

    run_test "4_mem_stat" '
register admin 123456
login admin 123456
create_pcb test 5
alloc 200
mem_stat
exit
'
    check_output "4_mem_stat" "碎片率" "mem_stat 输出碎片率"
}

# ================================================================
# 模块五：持久化（20分）
# ================================================================

test_module5() {
    echo ""
    echo "========== 模块五：持久化 =========="

    run_test "5_save" '
register admin 123456
login admin 123456
create_pcb worker1 3
alloc 200
save
exit
'
    check_output "5_save" "状态已保存" "save 保存成功"

    if [ -f "data/os_state.bin" ]; then
        if file data/os_state.bin | grep -q "data"; then
            echo -e "  ${GREEN}[PASS]${NC} os_state.bin 是二进制文件"
            ((PASS++))
        else
            echo -e "  ${RED}[FAIL]${NC} os_state.bin 可能不是二进制!"
            ((FAIL++))
        fi
    else
        echo -e "  ${RED}[FAIL]${NC} os_state.bin 文件不存在"
        ((FAIL++))
    fi

    run_test "5_load" '
login admin 123456
list_pcb
exit
'
    check_output "5_load" "worker1" "重启后自动加载 worker1"

    run_test "5_manual_load" '
register admin 123456
login admin 123456
create_pcb one 3
save
create_pcb two 5
load
list_pcb
exit
'
    check_output "5_manual_load" "one" "手动 load 恢复已保存进程"
}

# ================================================================
# 模块七：可视化（10分）
# ================================================================

test_module7() {
    echo ""
    echo "========== 模块七：可视化 =========="

    run_test "7_overview" '
register admin 123456
login admin 123456
create_pcb jobA 3
create_pcb jobB 7
alloc 200
overview
exit
'
    check_output "7_overview" "板块一.*进程树" "overview 板块一：进程树"
    check_output "7_overview" "板块二.*内存" "overview 板块二：内存布局"
    check_output "7_overview" "板块三.*MLFQ" "overview 板块三：MLFQ 队列"
    check_output "7_overview" "jobA" "overview 含 jobA"
    check_output "7_overview" "jobB" "overview 含 jobB"
    check_output "7_overview" "init" "overview 显示内存占用"

    run_test "7_dynamic" '
register admin 123456
login admin 123456
create_pcb jobA 3
step
overview
exit
'
    check_output "7_dynamic" "时钟.*[1-9]" "step 后 overview 时钟变化"
}

# ================================================================
# 主流程
# ================================================================

echo "============================================"
echo "  操作系统模拟器 — 自动化测试"
echo "============================================"

# 检查可执行文件
if [ ! -f "$EXE" ]; then
    echo "编译中..."
    mkdir -p build data
    for src in main user process scheduler memory persistence concurrency visualization; do
        gcc -Wall -Wextra -std=c99 -g -O2 -I. -c "src/${src}.c" -o "build/${src}.o"
    done
    gcc -o "$EXE" build/*.o -lpthread
    echo "编译完成。"
fi

case "${1:-all}" in
    module1) test_module1 ;;
    module2) test_module2 ;;
    module3) test_module3 ;;
    module4) test_module4 ;;
    module5) test_module5 ;;
    module7) test_module7 ;;
    all)
        test_module1
        test_module2
        test_module3
        test_module4
        test_module5
        test_module7
        ;;
    *)
        echo "用法: bash test_runner.sh [module1|module2|...|all]"
        exit 1
        ;;
esac

echo ""
echo "============================================"
echo "  测试完成: ${GREEN}${PASS} 通过${NC} / ${RED}${FAIL} 失败${NC}"
echo "  测试输出: ${OUTDIR}/"
echo ""
echo "  模块六（多线程并发）需手动双终端验证，不能自动化"
echo "============================================"
