#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import cv2
import numpy as np
import time
import json
import os
import socket
import sys
import argparse
from collections import deque
import signal
import threading
from tensorflow.keras.models import load_model

# 配置日志输出
import logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)
logger = logging.getLogger('GestureRecognizer')

class GestureUDPClient:
    """负责通过UDP发送手势识别结果到Qt应用"""
    def __init__(self, host='127.0.0.1', port=12345):
        self.host = host
        self.port = port
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        logger.info(f"UDP客户端初始化完成，目标: {host}:{port}")
        
    def send_gesture(self, gesture_name):
        """发送识别到的手势到Qt应用"""
        message = json.dumps({"gesture": gesture_name})
        try:
            self.socket.sendto(message.encode(), (self.host, self.port))
            logger.info(f"已发送手势: {gesture_name}")
            return True
        except Exception as e:
            logger.error(f"发送手势失败: {e}")
            return False

class ExitListener:
    """监听来自Qt应用的退出命令"""
    def __init__(self, port=12346):
        self.port = port
        self.should_exit = False
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            self.socket.bind(('127.0.0.1', port))
            self.socket.settimeout(0.5)  # 设置超时，确保不会阻塞主线程
            logger.info(f"退出监听器已绑定到端口 {port}")
        except Exception as e:
            logger.error(f"无法绑定到端口 {port}: {e}")
            raise
        
        # 启动监听线程
        self.listen_thread = threading.Thread(target=self._listen_for_exit)
        self.listen_thread.daemon = True
        self.listen_thread.start()
    
    def _listen_for_exit(self):
        """在后台线程中监听退出命令"""
        while True:
            try:
                data, addr = self.socket.recvfrom(1024)
                message = data.decode().strip()
                logger.info(f"收到消息: {message} 来自 {addr}")
                
                if message == "EXIT":
                    logger.info("收到退出命令，准备退出...")
                    self.should_exit = True
                    break
            except socket.timeout:
                # 超时是正常的，继续监听
                pass
            except Exception as e:
                logger.error(f"监听过程中发生错误: {e}")
                break
    
    def check_exit(self):
        """检查是否收到退出命令"""
        return self.should_exit
    
    def close(self):
        """关闭套接字"""
        try:
            self.socket.close()
            logger.info("退出监听器已关闭")
        except Exception as e:
            logger.error(f"关闭退出监听器时出错: {e}")

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
                return config.get('classes', ['swipe_left', 'click', 'neutral'])
        except FileNotFoundError:
            logger.warning("无法找到类别文件，使用默认类别")
            # 简化为只有需要的两个手势加上中性状态
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
        logger.info(f"使用图像尺寸: {self.img_size}")
        
        # 初始化预测历史
        self.history_size = 15  # 增加历史大小，使平滑效果更好
        self.prediction_history = deque(maxlen=self.history_size)
        
        # 手势状态跟踪
        self.current_gesture = None
        self.gesture_start_time = None
        self.gesture_count = {gesture: 0 for gesture in self.gestures}
        self.confidence_threshold = 0.7  # 根据需求设置为0.7
        self.min_detection_duration = 0.3  # 持续检测阈值（秒）
        
        # 防抖动参数
        self.last_triggered_time = 0
        self.cooldown_period = 1.0  # 触发手势后的冷却时间，避免频繁触发
        
        # 优化：预热模型
        self.warmup()
        
        logger.info(f"识别器初始化完成，可识别手势: {self.gestures}")
    
    def load_model(self, model_path):
        """加载模型，处理可能的错误"""
        try:
            model = load_model(model_path)
            logger.info(f"成功加载模型: {model_path}")
            return model
        except Exception as e:
            logger.error(f"无法加载模型 '{model_path}': {e}")
            # 尝试在多个位置查找模型
            alt_paths = [
                'gesture_model.h5'
             
            ]
            for path in alt_paths:
                try:
                    if os.path.exists(path):
                        logger.info(f"尝试加载备用模型: {path}")
                        model = load_model(path)
                        logger.info(f"成功加载备用模型: {path}")
                        return model
                except Exception as alt_e:
                    logger.error(f"加载备用模型失败: {alt_e}")
            
            raise ValueError(f"无法找到有效的手势识别模型")
    
    def load_config(self, config_path):
        """加载模型配置"""
        try:
            with open(config_path, 'r') as f:
                return json.load(f)
        except FileNotFoundError:
            logger.warning(f"配置文件不存在: {config_path}")
            return {"image_size": (128, 128)}
    
    def warmup(self):
        """预热模型，确保第一次预测时不会有延迟"""
        dummy_input = np.zeros((1, self.img_size[0], self.img_size[1], 1))
        self.model.predict(dummy_input, verbose=0)
        logger.info("模型预热完成")
    
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
              current_time - self.gesture_start_time > self.min_detection_duration and
              current_time - self.last_triggered_time > self.cooldown_period and
              smoothed_gesture != 'neutral'):  # 不触发中性状态
            
            # 只在持续检测到相同手势超过阈值时触发
            self.gesture_count[smoothed_gesture] += 1
            is_gesture_triggered = True
            self.last_triggered_time = current_time  # 更新最后触发时间
            logger.info(f"检测到 {smoothed_gesture} 手势! 置信度: {smoothed_confidence:.2f}")
            
        return {
            'roi': roi,
            'raw_prediction': (predicted_gesture, confidence),
            'smooth_prediction': (smoothed_gesture, smoothed_confidence),
            'is_triggered': is_gesture_triggered,
            'counts': self.gesture_count
        }
    
    def get_smoothed_prediction(self):
        """平滑预测结果，使用指数加权平均"""
        if not self.prediction_history:
            return None, 0
        
        # 最新的预测权重更高 (指数加权)
        weights = np.exp(np.linspace(0, 1, len(self.prediction_history)))
        weights = weights / np.sum(weights)  # 归一化权重
        
        # 计算加权平均
        avg_confidence = np.zeros(len(self.gestures))
        for i, pred in enumerate(self.prediction_history):
            avg_confidence += weights[i] * pred
        
        # 返回置信度最高的类别
        max_idx = np.argmax(avg_confidence)
        return self.gestures[max_idx], avg_confidence[max_idx]

def signal_handler(sig, frame):
    """处理CTRL+C等信号，确保程序优雅退出"""
    logger.info("接收到退出信号，程序退出中...")
    cv2.destroyAllWindows()
    sys.exit(0)

def find_camera():
    """查找可用的摄像头"""
    # 检查特定设备是否可用的辅助函数
    def check_device(index):
        # 优先尝试检测/dev/video{index}是否已被占用
        try:
            import subprocess
            result = subprocess.run(['fuser', f'/dev/video{index}'], 
                                   capture_output=True, text=True)
            if result.returncode == 0:
                logger.warning(f"设备 /dev/video{index} 已被占用")
                return False
        except Exception:
            pass
            
        # 尝试打开设备进行测试
        try:
            cap = cv2.VideoCapture(index)
            if cap.isOpened():
                w = cap.get(cv2.CAP_PROP_FRAME_WIDTH)
                h = cap.get(cv2.CAP_PROP_FRAME_HEIGHT)
                logger.info(f"找到可用摄像头 {index}，分辨率：{w}x{h}")
                cap.release()
                return True
            else:
                return False
        except Exception as e:
            return False
    
    # 尝试所有可能的摄像头索引
    for idx in range(10):  # 尝试0-9的索引
        if check_device(idx):
            return idx
    
    logger.error("未找到可用摄像头")
    return 0  # 默认值

def main():
    # 解析命令行参数
    parser = argparse.ArgumentParser(description='手势识别程序')
    parser.add_argument('--camera', type=int, default=-1, help='摄像头设备编号 (默认: 自动检测)')
    parser.add_argument('--model', type=str, default='gesture_model.h5', help='模型文件路径')
    parser.add_argument('--debug', action='store_true', help='启用调试模式（显示窗口）')
    parser.add_argument('--threshold', type=float, default=0.7, help='手势识别置信度阈值')
    args = parser.parse_args()
    
    # 设置信号处理器
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)
    
    # 自动检测摄像头
    camera_index = args.camera
    if camera_index < 0:
        camera_index = find_camera()
        
    # 初始化退出监听器
    try:
        exit_listener = ExitListener()
    except Exception as e:
        logger.error(f"无法初始化退出监听器: {e}")
        return 1
    
    # 初始化UDP客户端
    udp_client = GestureUDPClient()
    
    # 初始化识别器
    try:
        recognizer = GestureRecognizer(model_path=args.model)
        recognizer.confidence_threshold = args.threshold
        logger.info(f"识别器已配置，置信度阈值: {recognizer.confidence_threshold}")
    except Exception as e:
        logger.error(f"无法初始化手势识别器: {e}")
        exit_listener.close()
        return 1
    
    # 初始化摄像头
    cap = cv2.VideoCapture(camera_index)
    if not cap.isOpened():
        logger.error(f"错误: 无法打开摄像头 {camera_index}")
        exit_listener.close()
        return 1
    
    # 设置固定分辨率
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)

    # 获取摄像头分辨率
    frame_width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    frame_height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    logger.info(f"摄像头分辨率: {frame_width}x{frame_height}")

    # 计算ROI大小和位置 - 与训练时保持一致
    roi_size = 128  # 固定与训练集相同的尺寸
    roi_x = (frame_width - roi_size) // 2
    roi_y = (frame_height - roi_size) // 2
    
    # 如果启用调试模式，创建窗口
    if args.debug:
        cv2.namedWindow('手势识别', cv2.WINDOW_NORMAL)
        cv2.resizeWindow('手势识别', 800, 600)
        cv2.namedWindow('ROI', cv2.WINDOW_NORMAL)
        cv2.resizeWindow('ROI', 128, 128)
    
    # 帧率计算
    fps_start_time = time.time()
    fps_counter = 0
    fps = 0
    
    logger.info(f"开始实时手势识别，摄像头:{camera_index}, 调试模式:{args.debug}")

    try:
        # 主循环
        while True:
            # 检查是否收到退出命令
            if exit_listener.check_exit():
                logger.info("收到退出命令，程序退出中...")
                break
            
            # 读取摄像头
            ret, frame = cap.read()
            if not ret:
                logger.error("无法读取摄像头画面")
                # 尝试重新初始化摄像头
                cap.release()
                time.sleep(1)
                cap = cv2.VideoCapture(camera_index)
                if not cap.isOpened():
                    logger.error("无法恢复摄像头连接，程序退出")
                    break
                continue
            
            # 计算帧率
            fps_counter += 1
            if time.time() - fps_start_time > 1:
                fps = fps_counter
                fps_counter = 0
                fps_start_time = time.time()
                
            # 执行手势预测
            result = recognizer.predict(frame, roi_x, roi_y, roi_size)
            
            # 如果识别到手势并触发，发送到Qt应用
            if result['is_triggered']:
                gesture = result['smooth_prediction'][0]
                udp_client.send_gesture(gesture)
            
            # 如果启用调试模式，显示结果
            if args.debug:
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
                
                # 展示手势计数
                counts_text = " | ".join([f"{g}: {c}" for g, c in result['counts'].items()])
                cv2.putText(frame, counts_text, (10, 80), 
                          cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1)
                
                # 显示处理后的帧和ROI
                cv2.imshow('手势识别', frame)
                cv2.imshow('ROI', result['roi'])
                
                # 键盘控制
                key = cv2.waitKey(1) & 0xFF
                if key == ord('q'):
                    logger.info("用户按下Q键，程序退出")
                    break
                
            # 短暂睡眠以避免CPU过载
            time.sleep(0.01)
                
    except KeyboardInterrupt:
        logger.info("\n程序被用户中断")
    except Exception as e:
        logger.error(f"程序运行时出错: {e}", exc_info=True)
    finally:
        # 清理资源
        cap.release()
        if args.debug:
            cv2.destroyAllWindows()
        exit_listener.close()
        
        # 显示统计信息
        logger.info("\n手势识别统计:")
        for gesture, count in recognizer.gesture_count.items():
            if count > 0:  # 只显示有检测到的手势
                logger.info(f"  - {gesture}: 检测到 {count} 次")
        
        logger.info("程序已正常退出")
        return 0

if __name__ == "__main__":
    sys.exit(main())