import tensorflow as tf

gpus = tf.config.experimental.list_physical_devices('GPU')
if gpus:
    try:
        # 限制TensorFlow仅使用第一个GPU
        tf.config.experimental.set_visible_devices(gpus[0], 'GPU')
        logical_gpus = tf.config.experimental.list_logical_devices('GPU')
        print(len(gpus), "个物理GPU,", len(logical_gpus), "个逻辑GPU")
    except RuntimeError as e:
        # 必须在初始化GPU之前设置可见设备
        print(e)

a = tf.constant([1.0, 2.0, 3.0, 4.0, 5.0, 6.0], shape=[2, 3], name='a')
b = tf.constant([1.0, 2.0, 3.0, 4.0, 5.0, 6.0], shape=[3, 2], name='b')
c = tf.matmul(a, b)

print(c)

print(tf.config.list_physical_devices("GPU"))
