#include "ThreadPool.h"
#include <QCoreApplication>
#include <QElapsedTimer>

// 注册为Qt元对象系统
static int threadPoolMetaTypeId = qRegisterMetaType<ThreadPool*>("ThreadPool*");

// 监控线程池性能的统计类
class ThreadPoolStats : public QObject {
    
public:
    static ThreadPoolStats& instance() {
        static ThreadPoolStats instance;
        return instance;
    }
    
    void recordTaskDuration(qint64 microseconds) {
        QMutexLocker locker(&m_mutex);
        m_totalTasks++;
        m_totalDuration += microseconds;
        
        // 更新最大/最小时间
        m_maxDuration = qMax(m_maxDuration, microseconds);
        if (m_minDuration == 0 || microseconds < m_minDuration) {
            m_minDuration = microseconds;
        }
        
        // 计算每100个任务的平均值
        if (m_totalTasks % 100 == 0) {
            double avgMs = (m_totalDuration / static_cast<double>(m_totalTasks)) / 1000.0;
            qDebug() << "线程池性能统计 - 已处理任务:" << m_totalTasks
                     << "平均时间:" << avgMs << "ms"
                     << "最小:" << m_minDuration/1000.0 << "ms"
                     << "最大:" << m_maxDuration/1000.0 << "ms";
        }
    }
    
    // 重置统计
    void reset() {
        QMutexLocker locker(&m_mutex);
        m_totalTasks = 0;
        m_totalDuration = 0;
        m_maxDuration = 0;
        m_minDuration = 0;
    }
    
    // 获取平均任务处理时间（毫秒）
    double getAverageTaskTime() {
        QMutexLocker locker(&m_mutex);
        if (m_totalTasks == 0) return 0;
        return (m_totalDuration / static_cast<double>(m_totalTasks)) / 1000.0;
    }
    
private:
    ThreadPoolStats() : m_totalTasks(0), m_totalDuration(0), m_maxDuration(0), m_minDuration(0) {}
    ~ThreadPoolStats() {}
    
    QMutex m_mutex;
    qint64 m_totalTasks;
    qint64 m_totalDuration;
    qint64 m_maxDuration;
    qint64 m_minDuration;
    
    // 禁止复制
    ThreadPoolStats(const ThreadPoolStats&) = delete;
    ThreadPoolStats& operator=(const ThreadPoolStats&) = delete;
};

// 实现带性能监控的任务包装类
class MonitoredTask : public QRunnable {
public:
    MonitoredTask(QRunnable* task) : m_task(task) {
        setAutoDelete(true);
    }
    
    ~MonitoredTask() {
        // 如果任务不会自动删除，这里需要删除它
        if (!m_task->autoDelete()) {
            delete m_task;
        }
    }
    
    void run() override {
        QElapsedTimer timer;
        timer.start();
        
        // 执行原始任务
        m_task->run();
        
        // 记录执行时间
        qint64 elapsed = timer.nsecsElapsed() / 1000; // 转换为微秒
        ThreadPoolStats::instance().recordTaskDuration(elapsed);
    }
    
private:
    QRunnable* m_task;
};

// 添加用于自适应调整线程池大小的方法
void ThreadPool::adjustThreadCount() {
    int cpuCores = QThread::idealThreadCount();
    int currentThreads = threadCount();
    double avgTaskTime = ThreadPoolStats::instance().getAverageTaskTime();
    
    // 如果平均任务时间太长（超过16.7ms，约60FPS）且有足够CPU资源
    if (avgTaskTime > 16.7 && currentThreads < cpuCores) {
        setThreadCount(currentThreads + 1);
        qDebug() << "增加线程池大小以提高性能，新大小:" << threadCount();
    }
    // 如果平均任务时间很短且线程数大于最小值
    else if (avgTaskTime < 5.0 && currentThreads > 2) {
        setThreadCount(currentThreads - 1);
        qDebug() << "减少线程池大小以节省资源，新大小:" << threadCount();
    }
}