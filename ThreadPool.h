#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QQueue>
#include <QRunnable>
#include <QThreadPool>
#include <functional>
#include <atomic>
#include <QDebug>
#include <opencv2/opencv.hpp>
// 任务基类
class Task : public QRunnable {
public:
    Task() { setAutoDelete(true); }
    virtual ~Task() {}
    virtual void run() = 0;
};

// 函数任务类
class FunctionTask : public Task {
public:
    FunctionTask(std::function<void()> func) : m_function(func) {}
    
    void run() override {
        m_function();
    }
    
private:
    std::function<void()> m_function;
};

// 自定义线程池类
class ThreadPool : public QObject {
    Q_OBJECT
    
public:
    static ThreadPool& instance() {
        static ThreadPool instance;
        return instance;
    }
    
    // 设置线程池大小
    void setThreadCount(int count) {
        QThreadPool::globalInstance()->setMaxThreadCount(count);
        qDebug() << "线程池大小设置为:" << count << "线程";
    }
    
    // 获取当前线程池大小
    int threadCount() const {
        return QThreadPool::globalInstance()->maxThreadCount();
    }
    
    // 获取活动线程数
    int activeThreadCount() const {
        return QThreadPool::globalInstance()->activeThreadCount();
    }
    
    // 提交任务
    void enqueue(Task* task, int priority = 50) {
        QThreadPool::globalInstance()->start(task, priority);
    }
    
    // 提交函数作为任务
    void enqueue(std::function<void()> function) {
        QThreadPool::globalInstance()->start(new FunctionTask(function));
    }
    
    // 等待所有任务完成
    void waitForDone() {
        QThreadPool::globalInstance()->waitForDone();
    }
    
    // 等待所有任务完成，带超时
    bool waitForDone(int msTimeout) {
        return QThreadPool::globalInstance()->waitForDone(msTimeout);
    }
    
    // 添加自适应调整线程池大小的方法
    void adjustThreadCount();
    
private:
    ThreadPool() {
        // 默认线程数为CPU核心数
        int processorCount = QThread::idealThreadCount();
        // 对于RK3566，推荐使用少于核心总数的线程
        // RK3566有4个核心，使用2-3个线程以避免过度竞争
        int recommendedThreads = qMax(2, processorCount - 1);
        QThreadPool::globalInstance()->setMaxThreadCount(recommendedThreads);
        
        qDebug() << "初始化线程池 - 处理器核心数:" << processorCount 
                 << "，配置线程数:" << recommendedThreads;
                 
        // 设置线程过期时间
        QThreadPool::globalInstance()->setExpiryTimeout(30000); // 30秒线程过期时间
    }
    
    ~ThreadPool() {
        QThreadPool::globalInstance()->waitForDone();
        qDebug() << "线程池已销毁";
    }
    
    // 禁止复制和赋值
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
};

// 图像处理任务类
class ImageProcessTask : public Task {
public:
    ImageProcessTask(const cv::Mat& inputFrame, 
                    std::function<void(cv::Mat&)> processFunc,
                    std::function<void(const cv::Mat&)> resultCallback)
        : m_inputFrame(inputFrame.clone()),
          m_processFunc(processFunc),
          m_resultCallback(resultCallback) {}
    
    void run() override {
        // 处理图像
        m_processFunc(m_inputFrame);
        
        // 返回结果
        m_resultCallback(m_inputFrame);
    }
    
private:
    cv::Mat m_inputFrame;
    std::function<void(cv::Mat&)> m_processFunc;
    std::function<void(const cv::Mat&)> m_resultCallback;
};

// 定义优先级较高的帧处理任务类
class HighPriorityImageTask : public ImageProcessTask {
public:
    HighPriorityImageTask(const cv::Mat& inputFrame, 
                        std::function<void(cv::Mat&)> processFunc,
                        std::function<void(const cv::Mat&)> resultCallback)
        : ImageProcessTask(inputFrame, processFunc, resultCallback) 
    {
        // 高优先级任务由ThreadPool的enqueue方法处理
    }
};

#endif // THREADPOOL_H