#pragma once

// 第一步：先包含所有系统/引擎头文件
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"
#include "Containers/Queue.h"
#include "Sockets.h"

// 第二步：必须作为最后一个include（UE UHT强制规则）
#include "BPFL_TcpClient.generated.h"

// 前置声明：解决"未定义类型UBPFL_TcpClient"问题
class UBPFL_TcpClient;

// 声明日志类别（头文件中用EXTERN，cpp中定义）
DECLARE_LOG_CATEGORY_EXTERN(TCPClientLog, Log, All);

/**
 * TCP客户端状态结构体（线程安全，纯C++类型，不暴露给UE反射系统）
 * 修复：移除USTRUCT/GENERATED_BODY()，避免反射系统尝试拷贝不可拷贝类型
 */
struct FTcpClientState
{
    // 线程安全的状态标记
    FThreadSafeBool bIsConnected = false;
    FThreadSafeBool bIsAsyncReceiving = false;
    FThreadSafeBool bIsThreadRunning = false;

    // Socket资源
    FSocket* ClientSocket = nullptr;
    FCriticalSection SocketCriticalSection; // Socket操作临界区

    // 消息队列（多生产者单消费者模式）
    TQueue<FString, EQueueMode::Mpsc> MessageQueue;
    FCriticalSection MessageQueueCriticalSection; // 队列操作临界区

    // 超时配置（默认值）
    double ConnectionTimeout = 10.0;  // 连接超时（秒）
    double ThreadStopTimeout = 2.0;   // 线程停止超时（秒）

    // 时间戳
    double LastReceiveTime = 0.0;

    // 接收线程实例
    class FTCPReceiveRunnable* ReceiveRunnable = nullptr;
};

/**
 * TCP接收线程类（继承自FRunnable）
 */
class FTCPReceiveRunnable : public FRunnable
{
public:
    // 修复：统一用struct引用FTcpClientState（匹配声明类型）
    FTCPReceiveRunnable(struct FTcpClientState& InTcpState);
    // 析构函数
    virtual ~FTCPReceiveRunnable() override;

    // FRunnable接口
    virtual bool Init() override;
    virtual uint32 Run() override;
    virtual void Stop() override;

    // 安全停止线程
    void SafeStop();
    // 启动线程
    void Start();

private:
    // 线程实例
    FRunnableThread* Thread = nullptr;
    // 线程名称
    FString ThreadName;
    // 修复：用struct引用，且直接引用全局FTcpClientState（避免嵌套作用域问题）
    struct FTcpClientState& TcpState;
    // 线程停止标记
    FThreadSafeBool bShouldStop = false;
    // 线程操作临界区
    FCriticalSection ThreadCriticalSection;
};

/**
 * 蓝图函数库：TCP客户端工具
 */
UCLASS()
class FPS_NET_API UBPFL_TcpClient : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    // 获取TCP客户端状态（单例）
    static struct FTcpClientState& GetTcpClientState();

    /**
     * 连接到TCP服务器
     * @param IP 服务器IP地址
     * @param Port 服务器端口
     * @return 是否连接成功
     */
    UFUNCTION(BlueprintCallable, Category = "TCP|Client", meta = (DisplayName = "Connect to TCP Server"))
    static bool ConnectToServer(const FString& IP, int32 Port);

    /**
     * 断开与TCP服务器的连接
     */
    UFUNCTION(BlueprintCallable, Category = "TCP|Client", meta = (DisplayName = "Disconnect from TCP Server"))
    static void DisconnectFromServer();

    /**
     * 发送TCP消息
     * @param Message 要发送的消息
     * @return 是否发送成功
     */
    UFUNCTION(BlueprintCallable, Category = "TCP|Client", meta = (DisplayName = "Send TCP Message"))
    static bool SendTCPMessage(const FString& Message);

    /**
     * 启动异步接收线程
     */
    UFUNCTION(BlueprintCallable, Category = "TCP|Client", meta = (DisplayName = "Start Async Receive"))
    static void StartAsyncReceive();

    /**
     * 获取队列中的消息（从异步接收线程投递的消息）
     * @param OutMessage 输出的消息
     * @return 是否成功获取到消息
     */
    UFUNCTION(BlueprintCallable, Category = "TCP|Client", meta = (DisplayName = "Get Queued TCP Message"))
    static bool GetQueuedMessage(FString& OutMessage);

    /**
     * 判断是否已连接到服务器
     * @return 连接状态
     */
    UFUNCTION(BlueprintCallable, Category = "TCP|Client", meta = (DisplayName = "Is TCP Connected"))
    static bool IsConnected();

    /**
     * 测试直接接收Socket消息（非异步，仅用于调试）
     * @param OutMessage 输出的消息
     * @param OutBytesRead 读取的字节数
     * @return 是否接收成功
     */
    UFUNCTION(BlueprintCallable, Category = "TCP|Client", meta = (DisplayName = "Test Receive TCP Message"))
    static bool TestReceiveSocketMessage(FString& OutMessage, int32& OutBytesRead);
};