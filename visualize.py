import streamlit as st
import subprocess
import json
import pandas as pd
import re
import os
import time

# --- 1. 配置和初始化 ---
SIMULATOR_PATH = "./cpu.exe"

st.set_page_config(
    page_title="Y86-64 Simulator Visualization",
    layout="wide",
    initial_sidebar_state="auto"
)

# --- 2. 核心执行和解析函数 (保持不变) ---

@st.cache_data(show_spinner="正在执行模拟器并解析跟踪...")
def parse_simulator_output(yo_content: bytes):
    """运行C++模拟器，并解析其JSON状态输出和文本报告输出。"""
    if not os.path.exists(SIMULATOR_PATH):
        st.error(f"错误：未找到模拟器可执行文件：'{SIMULATOR_PATH}'。请确保您已编译C++代码并命名为 'simulator'。")
        return None, None
    
    try:
        yo_content_str = yo_content.decode('utf-8')
    except UnicodeDecodeError as e:
        st.error(f"无法解码文件内容为 UTF-8 字符串: {e}")
        return None, None

    try:
        result = subprocess.run(
            [SIMULATOR_PATH],
            input=yo_content_str,
            capture_output=True,
            text=True,
            check=False,
            timeout=10 
        )
    except Exception as e:
        st.error(f"执行模拟器失败: {e}")
        return None, None

    stdout_output = result.stdout.strip()
    stderr_output = result.stderr.strip()

    execution_trace = []
    try:
        if stdout_output and stdout_output.startswith("[") and stdout_output.endswith("]"):
            execution_trace = json.loads(stdout_output)
        
        if not execution_trace and stdout_output:
            last_brace = stdout_output.rfind('}')
            if last_brace != -1:
                clean_json = "[" + stdout_output[:last_brace+1] + "]"
                clean_json = clean_json.replace("}\n{", "},{").replace("}\r\n{", "},{")
                execution_trace = json.loads(clean_json)

    except json.JSONDecodeError:
        st.warning(f"JSON解析错误，可能程序提前中止或输出不完整。")
    
    reports = parse_reports(stderr_output)
    
    return execution_trace, reports

def parse_reports(stderr_output: str):
    """从 stderr 文本中提取 L1 Cache 和 ILP 报告数据。"""
    reports = {"Cache": {}, "ILP": {}}

    ilp_match = re.search(
        r"--- ILP Analysis Report ---\s*Total Instructions \(I\): (\d+)\s*Total Logic Cycles \(C\): (\d+)\s*Dynamic ILP \(I/C\): ([\d\.]+)\s*---",
        stderr_output, re.DOTALL
    )
    if ilp_match:
        reports["ILP"] = {
            "I": int(ilp_match.group(1)),
            "C": int(ilp_match.group(2)),
            "ILP": float(ilp_match.group(3))
        }

    cache_match = re.search(
        r"--- L1 Cache Report ---\s*Total Accesses: (\d+)\s*Hits: (\d+)\s*Misses: (\d+)\s*Hit Rate: ([\d\.]+)\%\s*---",
        stderr_output, re.DOTALL
    )
    if cache_match:
        reports["Cache"] = {
            "Accesses": int(cache_match.group(1)),
            "Hits": int(cache_match.group(2)),
            "Misses": int(cache_match.group(3)),
            "HitRate": float(cache_match.group(4))
        }
    return reports

# --- 3. 格式化和显示函数 (已修复 NameError) ---

def format_reg_value(val):
    """将int64值格式化为16进制字符串"""
    if isinstance(val, int):
        if val < 0:
            val = val & 0xFFFFFFFFFFFFFFFF
        return f'0x{val:016x}'
    return val

def format_mem_value(addr, val):
    """格式化内存地址和值"""
    # 修复：确保 addr 可以转换为 int
    addr_str = f'0x{int(addr):04x}' 
    if isinstance(val, int):
        if val < 0:
             val = val & 0xFFFFFFFFFFFFFFFF
        val_str = f'{val:016x}'
    else:
        val_str = str(val)
    return addr_str, val_str

def display_state(state, prev_state, step_id, max_steps):
    """显示单个执行步骤的CPU状态，使用稳定布局和条件高亮"""
    
    # 顶部状态区域: PC, CC, STAT
    col_pc, col_cc, col_stat = st.columns([1, 1, 1.5])

    stat_map = {1: "AOK (正常)", 2: "HLT (终止)", 3: "ADR (地址错误)", 4: "INS (指令错误)"}
    stat_desc = stat_map.get(state['STAT'], f"未知 ({state['STAT']})")
    
    # --- PC & Step ---
    with col_pc:
        st.markdown(f"**指令 ID:** <span style='font-size:1.2em; font-weight:bold;'>{step_id + 1} / {max_steps + 1}</span>", unsafe_allow_html=True)
        
        pc_val = f"0x{state['PC']:03x}"
        st.markdown(f"**PC:** `{pc_val}`")

    # --- 条件码 (CC) ---
    with col_cc:
        st.subheader("条件码 (CC)")
        cc = state['CC']
        prev_cc = prev_state['CC'] if prev_state else cc
        
        cc_data = {}
        for code in ['ZF', 'SF', 'OF']:
            val = cc[code]
            prev_val = prev_cc.get(code, val)
            color = 'red' if val != prev_val else 'white'
            cc_data[code] = f"<span style='color: {color};'>{val}</span>"

        cc_html = f"""
        | ZF | SF | OF |
        |---|---|---|
        | {cc_data['ZF']} | {cc_data['SF']} | {cc_data['OF']} |
        """
        st.markdown(cc_html, unsafe_allow_html=True)


    # --- STAT ---
    with col_stat:
        st.subheader("程序状态 (STAT)")
        st.markdown(f"<p style='font-size:1.2em; color: {'green' if state['STAT']==1 else 'red'}; font-weight:bold;'>{stat_desc}</p>", unsafe_allow_html=True)


    st.markdown("---")
    
    # --- 寄存器 (REG) ---
    st.subheader("寄存器 (Registers) - 变化高亮")
    
    reg_list = [
        'rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi',
        'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14'
    ]

    reg_data = {}
    prev_reg = prev_state['REG'] if prev_state else {}
    
    for k in reg_list:
        v = state['REG'].get(k)
        prev_v = prev_reg.get(k, v) 
        formatted_v = format_reg_value(v)
        formatted_prev_v = format_reg_value(prev_v)
        
        style = 'background-color: #33FF33; color: black; font-weight: bold;' if formatted_v != formatted_prev_v else ''
        reg_data[k] = f"<div style='{style} padding: 5px; border-radius: 3px;'>{formatted_v}</div>"

    cols_major = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi']
    cols_extra = ['r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14']
    
    def render_reg_table(cols):
        header = "".join([f"<th>{r.upper()}</th>" for r in cols])
        values = "".join([f"<td>{reg_data[r]}</td>" for r in cols])
        
        return f"""
        <table style="width: 100%; table-layout: fixed; margin-bottom: 10px;">
            <thead><tr>{header}</tr></thead>
            <tbody><tr>{values}</tr></tbody>
        </table>
        """
    
    st.markdown(render_reg_table(cols_major), unsafe_allow_html=True)
    st.markdown(render_reg_table(cols_extra), unsafe_allow_html=True)
    
    st.markdown("---")

    # --- 内存 (MEM) & 调试 ---
    col_mem, col_debug = st.columns([2, 1])

    with col_mem:
        with st.expander("展开内存状态 (Memory)", expanded=False):
            if state['MEM']:
                mem_data = {int(k): v for k, v in state['MEM'].items()}
                mem_data = sorted(mem_data.items())

                # 此处使用了 format_mem_value，现在已定义
                mem_data_fmt = [format_mem_value(addr, val) for addr, val in mem_data]
                mem_df = pd.DataFrame(mem_data_fmt, columns=['Address', 'Value'])
                mem_df['Value'] = '0x' + mem_df['Value'].str.upper() # 确保十六进制大写
                mem_df = mem_df.set_index('Address')
                
                st.dataframe(mem_df, height=250, width='stretch')
            else:
                st.info("内存当前无非零数据")

    with col_debug:
        with st.expander("原始 JSON 跟踪 (调试)", expanded=False):
            st.json(state)


def display_reports(reports):
    """显示最终的 ILP 和 Cache 报告 (与之前相同)"""
    st.header("✨ 最终分析报告")

    col_ilp, col_cache = st.columns(2)

    with col_ilp:
        st.subheader("指令级并行性 (ILP)")
        if reports["ILP"]:
            i = reports["ILP"]
            col1, col2, col3 = st.columns(3)
            col1.metric("总指令 (I)", i["I"])
            col2.metric("逻辑周期 (C)", i["C"])
            col3.metric("动态 ILP (I/C)", f'{i["ILP"]:.4f}')
        else:
            st.info("无 ILP 报告")

    with col_cache:
        st.subheader("L1 Cache 报告")
        if reports["Cache"]:
            c = reports["Cache"]
            col4, col5, col6 = st.columns(3)
            col4.metric("总访问次数", c["Accesses"])
            col5.metric("命中次数", c["Hits"])
            color = 'green' if c["HitRate"] > 80 else ('orange' if c["HitRate"] > 50 else 'red')
            col6.metric("命中率", f'{c["HitRate"]:.2f}%', delta_color="off")
            col6.markdown(f"<p style='color:{color}; font-weight:bold;'>{c['HitRate']:.2f}%</p>", unsafe_allow_html=True)
        else:
            st.info("无 Cache 报告")

def main():
    st.title("Y86-64 Simulator Visualization")
    # 初始化 session state
    if 'step' not in st.session_state:
        st.session_state.step = 0
        st.session_state.is_running = False
        st.session_state.max_steps = 0
        st.session_state.prev_state = None
        st.session_state.uploaded_file_name = None

    uploaded_file = st.sidebar.file_uploader("上传 .yo 机器码文件", type="yo")

    if uploaded_file is None:
        st.info("👈 请上传一个 .yo 文件开始模拟。")
        return

    # --- 执行模拟和解析 ---
    if st.session_state.uploaded_file_name != uploaded_file.name:
        st.session_state.clear()
        
        yo_content = uploaded_file.getvalue()
        trace, reports = parse_simulator_output(yo_content)
            
        if not trace:
            st.error("模拟器执行失败或未能解析任何有效的执行状态。")
            return
            
        st.session_state.trace = trace
        st.session_state.reports = reports
        st.session_state.uploaded_file_name = uploaded_file.name
        st.session_state.step = 0
        st.session_state.max_steps = len(trace) - 1
        st.session_state.prev_state = trace[0]
        st.session_state.is_running = False
        
        if st.session_state.max_steps > 0:
            st.success(f"模拟完成。共执行 {len(trace)} 条指令。")
        else:
            st.warning("程序只执行了 1 条指令（或未执行）。")

    
    trace = st.session_state.trace
    reports = st.session_state.reports
    max_steps = st.session_state.max_steps
    
    # --- 动态模拟和控制 ---
    st.sidebar.markdown("---")
    st.sidebar.header("动态执行控制")
    
    run_speed = st.sidebar.slider(
        "模拟速度 (秒/步)", min_value=0.1, max_value=2.0, value=0.5, step=0.1
    )

    col_btn1, col_btn2 = st.sidebar.columns(2)
    
    is_at_end = st.session_state.step >= max_steps
    if col_btn1.button("▶️ 自动运行", disabled=st.session_state.is_running or is_at_end):
        st.session_state.is_running = True
        st.session_state.step = min(st.session_state.step + 1, max_steps)
        st.rerun() 
    
    if col_btn2.button("⏹️ 停止/重置"):
        st.session_state.is_running = False
        st.session_state.step = 0
        st.rerun()
        
    current_step = st.sidebar.slider(
        "手动选择指令步数", min_value=0, max_value=max_steps,
        value=st.session_state.step, key='step_slider', disabled=st.session_state.is_running
    )
    st.session_state.step = current_step
    
    # --- 主显示区：指令执行状态 ---
    current_state = trace[st.session_state.step]
    current_prev_state = st.session_state.prev_state 

    display_state(current_state, current_prev_state, st.session_state.step, max_steps)

    st.session_state.prev_state = current_state
    
    # --- 自动运行循环逻辑 ---
    if st.session_state.is_running and st.session_state.step < max_steps:
        time.sleep(run_speed)
        st.session_state.step += 1
        st.rerun()

    # --- 最终报告 ---
    st.markdown("---")
    display_reports(reports)


if __name__ == "__main__":
    # 注入 CSS 以使排版更紧凑和高亮样式生效
    st.markdown("""
        <style>
            /* 移除 Streamlit 默认表格的边框，使用更简洁的样式 */
            .stDataFrame, .markdown-text-container table {
                font-size: 0.9em !important;
            }
            /* 自定义 HTML 表格样式 */
            table {
                border-collapse: collapse;
                width: 100%;
                font-family: monospace;
            }
            th, td {
                border: none !important; 
                text-align: center;
                padding: 0; 
            }
            /* 寄存器单元格的 div 样式，用于背景高亮 */
            td > div {
                font-size: 0.9em;
                word-wrap: break-word; 
                overflow: hidden;
                text-overflow: ellipsis;
            }
            /* 条件码表格的简单样式 */
            .markdown-text-container table {
                width: 50%;
                margin-left: 0;
            }
            .markdown-text-container table th, 
            .markdown-text-container table td {
                text-align: left;
                padding: 2px 5px;
            }
        </style>
    """, unsafe_allow_html=True)
    
    main()