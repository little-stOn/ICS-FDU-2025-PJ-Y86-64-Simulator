import subprocess
import re
import matplotlib.pyplot as plt
import numpy as np
import os
import sys

# =================配置区域=================

# 1. 你的可执行文件列表 (确保这些文件在当前目录下，或者写上绝对路径)
# 根据你的描述，你工作区有这三个文件
executables = {
    "No Cache": "./cpu_naive",
    "Direct Mapped\n(1-Way)": "./cpu_direct",
    "Set Associative\n(4-Way)": "./cpu"
}

# 2. 你的测试文件列表
# 如果你的 .yo 文件在 test 目录下，请修改这里，例如 "test/cache_conflict.yo"
benchmarks = [
    "test/cache_cmp.yo", 
    "test/code_data_conflict.yo"
]

# =================逻辑区域=================

def check_files_exist():
    """检查所有必要文件是否存在，不存在则报错退出"""
    missing = []
    
    # 检查可执行文件
    for name, cmd in executables.items():
        # 处理 ./ 前缀以进行路径检查
        path = cmd
        if not os.path.exists(path) and not os.path.exists(path + ".exe"):
            missing.append(f"可执行文件: {path}")
            
    # 检查 benchmark 文件
    for bench in benchmarks:
        if not os.path.exists(bench):
            missing.append(f"测试文件: {bench}")
            
    if missing:
        print("错误: 以下文件未找到，请检查路径或文件名:")
        for m in missing:
            print(f"  - {m}")
        print("\n提示: 如果 .yo 文件在 test/ 目录下，请修改脚本中的 benchmarks 列表。")
        sys.exit(1)

def run_simulation(exe_cmd, input_file):
    """
    模拟 Shell 命令: ./exe < input_file > /dev/null
    并捕获 stderr 中的报告
    """
    try:
        # 1. 模拟 '<' : 读取文件内容
        with open(input_file, 'r') as f:
            input_content = f.read()

        # 2. 运行进程
        # stdout=subprocess.DEVNULL 相当于 '> /dev/null' (丢弃 JSON 输出)
        # stderr=subprocess.PIPE 捕获性能报告
        process = subprocess.run(
            [exe_cmd],
            input=input_content,
            text=True,
            stdout=subprocess.DEVNULL, 
            stderr=subprocess.PIPE
        )
        
        return process.stderr

    except Exception as e:
        print(f"执行出错 {exe_cmd}: {e}")
        return ""

def parse_miss_rate(output_text):
    """从 stderr 文本中正则提取 Miss Rate"""
    # 匹配 cpu.cpp 输出的 "Hit Rate: 95.3991%"
    hit_match = re.search(r"Hit Rate:\s+(\d+\.\d+)%", output_text)
    if hit_match:
        hit_rate = float(hit_match.group(1))
        return 100.0 - hit_rate
    
    # 如果没找到 Hit Rate (比如 naive 版本没输出这个)，默认 Miss 100%
    return 100.0

# --- 主程序 ---

print(f"当前工作目录: {os.getcwd()}")
check_files_exist() # 先自检

data = {} 

print("\n开始自动化测试...")

for bench in benchmarks:
    miss_rates = []
    print(f"\n--- 正在测试 Benchmark: {bench} ---")
    
    for name, cmd in executables.items():
        # 运行模拟
        output = run_simulation(cmd, bench)
        
        # 解析结果
        miss_rate = parse_miss_rate(output)
        miss_rates.append(miss_rate)
        
        print(f"  [{name.replace(chr(10), ' ')}] -> Miss Rate: {miss_rate:.2f}%")
        
        # 调试: 如果结果是 100% 且不是 naive，可能是解析失败，打印输出来检查
        if miss_rate == 100.0 and "Naive" not in name:
            # 简单检查输出是否为空
            if len(output) < 10:
                print(f"    [警告] 输出为空，可能程序运行失败或路径错误。")

    data[bench] = miss_rates

# =================绘图区域=================
print("\n正在生成图表...")

x = np.arange(len(benchmarks))
width = 0.25
multiplier = 0

fig, ax = plt.subplots(figsize=(10, 6))
colors = ['#e74c3c', '#f1c40f', '#2ecc71'] 

# 转置数据以按策略分组绘图
strategies_data = list(zip(*data.values()))

for i, (strategy_name, strategy_vals) in enumerate(zip(executables.keys(), strategies_data)):
    offset = width * multiplier
    rects = ax.bar(x + offset, strategy_vals, width, label=strategy_name.replace('\n', ' '), color=colors[i % 3], alpha=0.9)
    
    # 标注数值
    for rect in rects:
        height = rect.get_height()
        # 智能位置：如果柱子太低，文字标在上面一点
        xytext_offset = (0, 3)
        ax.annotate(f'{height:.1f}%',
                    xy=(rect.get_x() + rect.get_width() / 2, height),
                    xytext=xytext_offset, 
                    textcoords="offset points",
                    ha='center', va='bottom', fontweight='bold', fontsize=9)
    multiplier += 1

ax.set_ylabel('Miss Rate (%)', fontsize=12, fontweight='bold')
ax.set_title('Cache Performance Comparison', fontsize=14, fontweight='bold', pad=20)
ax.set_xticks(x + width)
# 简化X轴标签，只取文件名
ax.set_xticklabels([b.split('/')[-1] for b in benchmarks], fontsize=10)
ax.set_ylim(0, 115)
ax.legend(loc='upper right', fontsize=10)
ax.yaxis.grid(True, linestyle='--', alpha=0.5)

output_filename = 'cache_performance_viz.png'
plt.tight_layout()
plt.savefig(output_filename, dpi=300)
print(f"图表已保存为: {output_filename}")
# plt.show() # 如果在服务器或无图形界面环境，注释掉此行