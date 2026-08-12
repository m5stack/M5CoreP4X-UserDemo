
import os
import re
import shutil

def update_cmake_file():
    # 获取脚本所在的目录
    script_dir = os.path.dirname(os.path.abspath(__file__))
    print(f"脚本所在目录: {script_dir}")

    # 核心目录
    output_dir = os.path.join(script_dir, "corep4_lvgl_ui_out")
    ui_source_dir = os.path.join(output_dir, "corep4_lvgl_ui", "ui")

    # ===== 步骤 1: 删除 output_dir 下除了 corep4_lvgl_ui 外的文件/文件夹 =====
    print(f"\n步骤1: 清理 {output_dir} 目录...")
    for item in os.listdir(output_dir):
        item_path = os.path.join(output_dir, item)
        if item != "corep4_lvgl_ui" and os.path.exists(item_path):
            if os.path.isfile(item_path) or os.path.islink(item_path):
                os.remove(item_path)
                print(f"  删除文件: {item}")
            else:
                shutil.rmtree(item_path)
                print(f"  删除文件夹: {item}")

    # ===== 步骤 2: 复制 ui_source_dir 下的内容到 output_dir =====
    print(f"\n步骤2: 从 {ui_source_dir} 复制文件到 {output_dir}...")
    if not os.path.exists(ui_source_dir):
        print(f"错误: 源目录不存在 '{ui_source_dir}'")
        return

    for item in os.listdir(ui_source_dir):
        src = os.path.join(ui_source_dir, item)
        dst = os.path.join(output_dir, item)
        if os.path.isfile(src) or os.path.islink(src):
            shutil.copy2(src, dst)
            print(f"  复制文件: {item}")
        else:
            shutil.copytree(src, dst, dirs_exist_ok=True)
            print(f"  复制文件夹: {item}")

    # ===== 步骤 3: 修改 CMakeLists.txt =====
    print(f"\n步骤3: 更新 CMakeLists.txt...")
    target_file = os.path.join(output_dir, "CMakeLists.txt")

    if not os.path.exists(target_file):
        print(f"错误: CMakeLists.txt 不存在于 '{target_file}'")
        return

    # 正则表达式，用于匹配 add_library(name ${VAR})
    regex = r'add_library\s*\(\s*\w+\s+([^\)]+)\s*\)'

    try:
        with open(target_file, 'r', encoding='utf-8') as f:
            content = f.read()
            print("成功读取文件内容。")
    except Exception as e:
        print(f"读取文件时出错: {e}")
        return

    match = re.search(regex, content)

    if match:
        print("找到 'add_library' 命令，正在执行替换...")
        sources_var = match.group(1)
        original_command = match.group(0)

        new_block = f"""# {original_command}

idf_component_register(SRCS {sources_var}
                       INCLUDE_DIRS "."
                       REQUIRES lvgl
                       )"""

        if original_command in content:
            new_content = content.replace(original_command, new_block)
            try:
                with open(target_file, 'w', encoding='utf-8') as f:
                    f.write(new_content)
                print(f"成功更新文件: {target_file}")
            except Exception as e:
                print(f"写入文件时出错: {e}")
        else:
            print("内容中未找到原始命令，可能已被修改。")

    else:
        print("在文件中未找到 'add_library(...)' 命令，或文件已被修改。")

    print("\n完成!")

if __name__ == "__main__":
    update_cmake_file()
