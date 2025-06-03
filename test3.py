import cv2
import numpy as np
import time
import json
import os
from collections import deque
from tensorflow.keras.models import load_model

def load_gesture_classes(file_path='gesture_classes.txt'):
    """加载手势类别"""
    try:
        with open(file_path, 'r') as f:
            return [line.strip() for line in f.readlines()]
    except FileNotFoundError:
        # 如果文件不存在，尝试查找model_config.json
        try:
            with open('model_config.json', 'r') as f:
                config = json.load(f)
                return config.get('classes', ['swipe_left',  'click', 'neutral'])
        except FileNotFoundError:
            print(f"警告: 无法找到类别文件，使用默认类别")
            return ['swipe_left', 'click', 'neutral']

def process_frame(frame, roi_x, roi_y, roi_size, img_size=(128, 128)):
    """预处理图像帧，保持与训练时一致的处理方式"""
    # 提取ROI
    roi = frame[roi_y:roi_y+roi_size, roi_x:roi_x+roi_size].copy()
    
    # 转为灰度
    gray_roi = cv2.cvtColor(roi, cv2.COLOR_BGR2GRAY)
    
    # 直方图均衡化（与训练时保持一致）
    gray_roi = cv2.equalizeHist(gray_roi)
    
    # 调整大小为模型输入尺寸
    resized_roi = cv2.resize(gray_roi, img_size)
    
    # 归一化
    normalized_roi = resized_roi / 255.0
    
    # 添加批次和通道维度
    input_data = np.expand_dims(np.expand_dims(normalized_roi, axis=-1), axis=0)
    
    return roi, input_data

class GestureRecognizer:
    def __init__(self, model_path='gesture_model.h5', class_path='gesture_classes.txt', config_path='model_config.json'):
        # 加载模型和类别
        self.model = self.load_model(model_path)
        self.gestures = load_gesture_classes(class_path)
        self.config = self.load_config(config_path)
        
        # 获取图像尺寸
        self.img_size = self.config.get('image_size', (128, 128))
        print(f"使用图像尺寸: {self.img_size}")
        
        # 初始化预测历史
        self.history_size = 10
        self.prediction_history = deque(maxlen=self.history_size)
        
        # 手势状态跟踪
        self.current_gesture = None
        self.gesture_start_time = None
        self.gesture_count = {gesture: 0 for gesture in self.gestures}
        self.confidence_threshold = 0.6  # 降低阈值增加灵敏度
        self.min_detection_duration = 0.3  # 持续检测阈值（秒）
        
        print(f"识别器初始化完成，可识别手势: {self.gestures}")
    
    def load_model(self, model_path):
        """加载模型，处理可能的错误"""
        try:
            model = load_model(model_path)
            print(f"成功加载模型: {model_path}")
            return model
        except Exception as e:
            print(f"错误: 无法加载模型 '{model_path}': {e}")
            raise
    
    def load_config(self, config_path):
        """加载模型配置"""
        try:
            with open(config_path, 'r') as f:
                return json.load(f)
        except FileNotFoundError:
            print(f"警告: 配置文件不存在: {config_path}")
            return {"image_size": (128, 128)}
    
    def predict(self, frame, roi_x, roi_y, roi_size):
        """预测单帧的手势"""
        roi, input_data = process_frame(frame, roi_x, roi_y, roi_size, self.img_size)
        
        # 模型预测
        predictions = self.model.predict(input_data, verbose=0)[0]
        predicted_class_idx = np.argmax(predictions)
        confidence = predictions[predicted_class_idx]
        predicted_gesture = self.gestures[predicted_class_idx]
        
        # 更新预测历史
        self.prediction_history.append(predictions)
        
        # 获取平滑后的结果
        smoothed_gesture, smoothed_confidence = self.get_smoothed_prediction()
        
        # 更新手势状态
        current_time = time.time()
        is_gesture_triggered = False
        
        if smoothed_gesture != self.current_gesture:
            self.current_gesture = smoothed_gesture
            self.gesture_start_time = current_time
        elif (smoothed_gesture == self.current_gesture and 
              smoothed_confidence > self.confidence_threshold and
              self.gesture_start_time is not None and
              current_time - self.gesture_start_time > self.min_detection_duration):
            # 只在持续检测到相同手势超过阈值时触发
            self.gesture_count[smoothed_gesture] += 1
            is_gesture_triggered = True
            print(f"检测到 {smoothed_gesture} 手势! 置信度: {smoothed_confidence:.2f}")
            # 重置计时器，避免连续触发
            self.gesture_start_time = current_time
        
        return {
            'roi': roi,
            'raw_prediction': (predicted_gesture, confidence),
            'smooth_prediction': (smoothed_gesture, smoothed_confidence),
            'is_triggered': is_gesture_triggered,
            'counts': self.gesture_count
        }
    
    def get_smoothed_prediction(self):
        """平滑预测结果，使用加权平均"""
        if not self.prediction_history:
            return None, 0
        
        # 最新的预测权重更高
        weights = np.linspace(0.5, 1.0, len(self.prediction_history))
        weights = weights / np.sum(weights)  # 归一化权重
        
        # 计算加权平均
        avg_confidence = np.zeros(len(self.gestures))
        for i, pred in enumerate(self.prediction_history):
            avg_confidence += weights[i] * pred
        
        # 返回置信度最高的类别
        max_idx = np.argmax(avg_confidence)
        return self.gestures[max_idx], avg_confidence[max_idx]

def main():
    # 初始化识别器
    try:
        recognizer = GestureRecognizer()
    except Exception as e:
        print(f"无法初始化手势识别器: {e}")
        return
    
    # 初始化摄像头
    cap = cv2.VideoCapture(0)
    if not cap.isOpened():
        print("错误: 无法打开摄像头")
        return
    
    # 设置固定分辨率
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)

    # 获取摄像头分辨率
    frame_width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    frame_height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))

    # 计算ROI大小和位置 - 与训练时保持一致
    roi_size = 128  # 固定与训练集相同的尺寸
    roi_x = (frame_width - roi_size) // 2
    roi_y = (frame_height - roi_size) // 2
    
    # 创建固定大小窗口
    cv2.namedWindow('手势识别', cv2.WINDOW_NORMAL)
    cv2.resizeWindow('手势识别', 800, 600)
    
    # 创建实时图表窗口
    chart_height = 200
    chart_width = 400
    confidence_chart = np.ones((chart_height, chart_width, 3), dtype=np.uint8) * 255
    
    # 颜色映射，为每个手势分配不同颜色
    colors = [
        (255, 0, 0),    # 蓝色
        (0, 255, 0),    # 绿色
        (0, 0, 255),    # 红色
        (255, 255, 0),  # 青色
        (255, 0, 255),  # 粉色
        (0, 255, 255),  # 黄色
    ]
    
    # 帧率计算
    fps_start_time = time.time()
    fps_counter = 0
    fps = 0
    
    print("开始实时手势识别...")
    print("按 'q' 键退出")
    print("按 's' 键保存当前帧")
    print("按 't' 键调整置信度阈值")

    try:
        while True:
            ret, frame = cap.read()
            if not ret:
                print("无法读取摄像头画面")
                break
            
            # 计算帧率
            fps_counter += 1
            if time.time() - fps_start_time > 1:
                fps = fps_counter
                fps_counter = 0
                fps_start_time = time.time()
                
            # 执行手势预测
            result = recognizer.predict(frame, roi_x, roi_y, roi_size)
            
            # 绘制ROI区域
            cv2.rectangle(frame, (roi_x, roi_y), (roi_x+roi_size, roi_y+roi_size), (0, 255, 0), 2)
            
            # 解包预测结果
            raw_gesture, raw_confidence = result['raw_prediction']
            smooth_gesture, smooth_confidence = result['smooth_prediction']
            triggered = result['is_triggered']
            
            # 显示FPS和阈值
            cv2.putText(frame, f"FPS: {fps}", (10, 25), 
                      cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 2)
            cv2.putText(frame, f"阈值: {recognizer.confidence_threshold:.2f}", (10, 50), 
                      cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 2)
            
            # 显示原始和平滑预测结果
            cv2.putText(frame, f"原始: {raw_gesture} ({raw_confidence:.2f})", 
                      (10, frame_height - 60), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2)
            cv2.putText(frame, f"平滑: {smooth_gesture} ({smooth_confidence:.2f})", 
                      (10, frame_height - 30), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 0, 0), 2)
            
            # 显示触发状态
            if triggered:
                cv2.putText(frame, f"检测到: {smooth_gesture}!", 
                          (roi_x, roi_y - 10), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 0, 255), 2)
                
                # 在ROI周围绘制高亮边框
                cv2.rectangle(frame, (roi_x-5, roi_y-5), 
                            (roi_x+roi_size+5, roi_y+roi_size+5), (0, 0, 255), 3)
            
            # 更新置信度图表
            confidence_chart = np.roll(confidence_chart, -1, axis=1)
            confidence_chart[:, -1, :] = 255  # 清除最右列
            
            # 为每个手势绘制置信度曲线
            if len(recognizer.prediction_history) > 0:
                latest_pred = recognizer.prediction_history[-1]
                for i, conf in enumerate(latest_pred):
                    if i < len(colors):
                        y = chart_height - int(conf * chart_height)
                        y = max(0, min(y, chart_height-1))  # 确保在有效范围内
                        confidence_chart[y, -1, :] = colors[i]
            
            # 在图表上标注手势名称
            for i, gesture in enumerate(recognizer.gestures):
                if i < len(colors):
                    cv2.putText(confidence_chart, gesture, (10, 20 + i*20), 
                              cv2.FONT_HERSHEY_SIMPLEX, 0.5, colors[i], 1)
            
            # 展示手势计数
            counts_text = " | ".join([f"{g}: {c}" for g, c in result['counts'].items()])
            cv2.putText(frame, counts_text, (10, 80), 
                      cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1)
            
            # 显示处理后的帧和ROI
            cv2.imshow('手势识别', frame)
            cv2.imshow('ROI', result['roi'])
            cv2.imshow('置信度图表', confidence_chart)
            
            # 键盘控制
            key = cv2.waitKey(1) & 0xFF
            
            if key == ord('q'):
                break
            elif key == ord('s'):
                # 保存当前帧和ROI
                timestamp = time.strftime("%Y%m%d-%H%M%S")
                cv2.imwrite(f"capture_frame_{timestamp}.jpg", frame)
                cv2.imwrite(f"capture_roi_{timestamp}.jpg", result['roi'])
                print(f"已保存当前帧到 capture_frame_{timestamp}.jpg")
            elif key == ord('t'):
                # 调整置信度阈值
                new_threshold = float(input("输入新的置信度阈值 (0.0-1.0): "))
                recognizer.confidence_threshold = max(0.1, min(0.9, new_threshold))
                print(f"置信度阈值已调整为: {recognizer.confidence_threshold}")
                
    except KeyboardInterrupt:
        print("\n程序被用户中断")
    finally:
        cap.release()
        cv2.destroyAllWindows()
        
        # 显示统计信息
        print("\n手势识别统计:")
        for gesture, count in recognizer.gesture_count.items():
            print(f"  - {gesture}: 检测到 {count} 次")

if __name__ == "__main__":
    main()