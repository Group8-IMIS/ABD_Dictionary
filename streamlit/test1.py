import streamlit as st

# Streamlit 标题
st.title("简单的Streamlit示例")

# 添加文本
st.write("这是一个简单的Streamlit示例。")

# 添加一个互动按钮
if st.button("点击我"):
    st.write("按钮被点击了！")

# 添加一个文本输入框
user_input = st.text_input("请输入一些文本:")
st.write("你输入的文本是:", user_input)

# 添加一个下拉框
options = ["选项1", "选项2", "选项3"]
selected_option = st.selectbox("选择一个选项:", options)
st.write("你选择的选项是:", selected_option)

# 添加一个多选框
if st.checkbox("显示详细信息"):
    st.write("这是一些详细信息。")

# 添加一个图表
import matplotlib.pyplot as plt
import numpy as np

x = np.linspace(0, 10, 100)
y = np.sin(x)
fig, ax = plt.subplots()
ax.plot(x, y)
st.pyplot(fig)

# 添加数据框
import pandas as pd

data = pd.DataFrame({
    '名字': ['Alice', 'Bob', 'Charlie'],
    '年龄': [25, 30, 35]
})
st.write(data)
