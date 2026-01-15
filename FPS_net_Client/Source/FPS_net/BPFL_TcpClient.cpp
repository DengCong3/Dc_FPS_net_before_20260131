// 核心：第一个包含项必须是自身头文件（UE强制规则）
#include "BPFL_TcpClient.h"

// UE5.7 TCP/Socket相关核心头文件
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"
#include "Misc/DateTime.h"
#include "HAL/RunnableThread.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/PlatformTime.h"

// 定义日志类别（匹配头文件的DECLARE_LOG_CATEGORY_EXTERN）
DEFINE_LOG_CATEGORY(TCPClientLog);

// 前置声明辅助函数：Socket错误码转字符串
static FString GetSocketErrorDescription(ESocketErrors ErrorCode);

// ========== FTCPReceiveRunnable 实现 ==========
FTCPReceiveRunnable::FTCPReceiveRunnable(struct FTcpClientState& InTcpState)
    : Thread(nullptr)
    , ThreadName(FString::Printf(TEXT("TCPClientReceiveThread_%lld"), FDateTime::Now().ToUnixTimestamp()))
    , TcpState(InTcpState)
    , bShouldStop(false)
{
}

FTCPReceiveRunnable::~FTCPReceiveRunnable()
{
    if (Thread && TcpState.bIsThreadRunning)
    {
        SafeStop();
    }
}

bool FTCPReceiveRunnable::Init()
{
    TcpState.bIsThreadRunning = true;
    TcpState.LastReceiveTime = FPlatformTime::Seconds();
    bShouldStop = false;
    UE_LOG(TCPClientLog, Log, TEXT("[TCP] 接收线程[%s]初始化成功"), *ThreadName);
    return true;
}

uint32 FTCPReceiveRunnable::Run()
{
    TArray<uint8> RecvBuffer;
    RecvBuffer.SetNumUninitialized(4096);
    const float SleepInterval = 0.02f; // 降低CPU占用：20ms休眠

    while (!bShouldStop && TcpState.bIsThreadRunning && TcpState.bIsConnected)
    {
        // 检查Socket有效性
        FSocket* CurrentSocket = nullptr;
        {
            FScopeLock Lock(&TcpState.SocketCriticalSection);
            CurrentSocket = TcpState.ClientSocket;
            if (!CurrentSocket || !TcpState.bIsConnected)
            {
                break;
            }
        }

        // 连接超时检测
        const double CurrentTime = FPlatformTime::Seconds();
        if (CurrentTime - TcpState.LastReceiveTime > TcpState.ConnectionTimeout)
        {
            UE_LOG(TCPClientLog, Error, TEXT("[TCP] 连接超时（%f秒无数据）"), TcpState.ConnectionTimeout);
            TcpState.bIsConnected = false;
            break;
        }

        // 非阻塞接收数据
        int32 BytesRead = 0;
        bool bRecvSuccess = CurrentSocket->Recv(RecvBuffer.GetData(), RecvBuffer.Num(), BytesRead);

        // 处理接收结果
        if (bRecvSuccess && BytesRead > 0)
        {
            TcpState.LastReceiveTime = CurrentTime;
            // 安全转换字符串（避免内存越界）
            FString RecvMsg;
            RecvMsg.AppendChars(reinterpret_cast<const TCHAR*>(UTF8_TO_TCHAR(RecvBuffer.GetData())), BytesRead);
            RecvMsg = RecvMsg.TrimStartAndEnd();

            if (!RecvMsg.IsEmpty())
            {
                // 投递消息到GameThread
                FFunctionGraphTask::CreateAndDispatchWhenReady([this, RecvMsg]()
                    {
                        FScopeLock QueueLock(&TcpState.MessageQueueCriticalSection);
                        TcpState.MessageQueue.Enqueue(RecvMsg);
                        UE_LOG(TCPClientLog, Log, TEXT("[TCP] 收到消息：%s"), *RecvMsg);
                    }, TStatId(), nullptr, ENamedThreads::GameThread);
            }
            // 清空已读取的缓冲区
            FMemory::Memzero(RecvBuffer.GetData(), BytesRead);
        }
        else if (bRecvSuccess && BytesRead == 0)
        {
            UE_LOG(TCPClientLog, Verbose, TEXT("[TCP] 收到空数据（非断开）"));
        }
        else
        {
            const int32 SE_WOULDBLOCK = 10035;
            ESocketErrors SocketErr = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode();
            if ((int32)SocketErr == SE_WOULDBLOCK)
            {
                FPlatformProcess::Sleep(SleepInterval);
                continue;
            }
            else if (SocketErr == SE_ECONNRESET || SocketErr == SE_ESHUTDOWN)
            {
                UE_LOG(TCPClientLog, Error, TEXT("[TCP] 服务器断开：%s"), *GetSocketErrorDescription(SocketErr));
                TcpState.bIsConnected = false;
                break;
            }
            else
            {
                UE_LOG(TCPClientLog, Warning, TEXT("[TCP] 接收异常：%s"), *GetSocketErrorDescription(SocketErr));
                FPlatformProcess::Sleep(0.1f);
            }
        }

        FPlatformProcess::Sleep(SleepInterval);
    }

    // 线程退出时重置状态
    TcpState.bIsAsyncReceiving = false;
    TcpState.bIsThreadRunning = false;
    bShouldStop = false;
    UE_LOG(TCPClientLog, Log, TEXT("[TCP] 接收线程[%s]退出"), *ThreadName);
    return 0;
}

void FTCPReceiveRunnable::SafeStop()
{
    FScopeLock Lock(&ThreadCriticalSection);
    // 1. 标记停止（先禁止线程继续执行）
    bShouldStop = true;
    TcpState.bIsThreadRunning = false;
    TcpState.bIsAsyncReceiving = false;

    // 2. 安全关闭Socket（核心修改：彻底销毁Socket，避免残留）
    {
        FScopeLock SocketLock(&TcpState.SocketCriticalSection);
        TcpState.bIsConnected = false;

        if (TcpState.ClientSocket)
        {
            // 修改1：先关闭读写流（原仅关闭读流，导致资源残留）
            TcpState.ClientSocket->Shutdown(ESocketShutdownMode::ReadWrite);
            // 修改2：关闭Socket后，通过子系统彻底销毁（原仅Close未销毁）
            TcpState.ClientSocket->Close();
            ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(TcpState.ClientSocket);
            TcpState.ClientSocket = nullptr; // 修改3：置空指针，避免野指针访问
        }
    }

    // 3. 线程安全释放（保持原逻辑，确保线程彻底退出）
    if (Thread)
    {
        // Kill(true)：阻塞直到线程退出，避免资源竞争
        Thread->Kill(true);
        delete Thread;
        Thread = nullptr; // 确保指针置空，防止重复释放
        UE_LOG(TCPClientLog, Log, TEXT("[TCP] 接收线程[%s]已强制终止"), *ThreadName);
    }

    UE_LOG(TCPClientLog, Log, TEXT("[TCP] 接收线程[%s]已完全停止（资源已释放）"), *ThreadName);
}

void FTCPReceiveRunnable::Stop()
{
    SafeStop();
}

void FTCPReceiveRunnable::Start()
{
    FScopeLock Lock(&ThreadCriticalSection);
    if (!Thread && TcpState.bIsConnected && !bShouldStop)
    {
        // 最低优先级线程，避免抢占GameThread
        Thread = FRunnableThread::Create(this, *ThreadName, 0, TPri_Lowest);
        UE_LOG(TCPClientLog, Log, TEXT("[TCP] 接收线程[%s]启动"), *ThreadName);
    }
}

// ========== UBPFL_TcpClient 实现 ==========
struct FTcpClientState& UBPFL_TcpClient::GetTcpClientState()
{
    static struct FTcpClientState TcpState;
    return TcpState;
}

bool UBPFL_TcpClient::ConnectToServer(const FString& IP, int32 Port)
{
    struct FTcpClientState& TcpState = GetTcpClientState();
    FScopeLock Lock(&TcpState.SocketCriticalSection);

    // 先断开旧连接
    DisconnectFromServer();

    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!SocketSubsystem)
    {
        UE_LOG(TCPClientLog, Error, TEXT("[TCP] 无法获取Socket子系统"));
        return false;
    }

    // 解析IP地址
    FIPv4Address IPv4Addr;
    if (!FIPv4Address::Parse(IP, IPv4Addr))
    {
        UE_LOG(TCPClientLog, Error, TEXT("[TCP] IP解析失败：%s"), *IP);
        return false;
    }

    // 配置服务器地址
    TSharedRef<FInternetAddr> ServerAddr = SocketSubsystem->CreateInternetAddr();
    ServerAddr->SetIp(IPv4Addr.Value);
    ServerAddr->SetPort(Port);

    // 创建Socket
    FString SocketName = FString::Printf(TEXT("TCPClientSocket_%lld"), FDateTime::Now().ToUnixTimestamp());
    TcpState.ClientSocket = SocketSubsystem->CreateSocket(NAME_Stream, *SocketName, false);
    if (!TcpState.ClientSocket)
    {
        UE_LOG(TCPClientLog, Error, TEXT("[TCP] 创建Socket失败"));
        return false;
    }

    // Socket配置
    TcpState.ClientSocket->SetLinger(true, 0);
    TcpState.ClientSocket->SetNoDelay(true);
    const int32 BufSize = 8192;
    int32 NewRecvBufSize = BufSize;
    int32 NewSendBufSize = BufSize;
    TcpState.ClientSocket->SetReceiveBufferSize(BufSize, NewRecvBufSize);
    TcpState.ClientSocket->SetSendBufferSize(BufSize, NewSendBufSize);

    // 阻塞连接（连接成功后改为非阻塞）
    TcpState.ClientSocket->SetNonBlocking(false);
    bool bConnected = TcpState.ClientSocket->Connect(*ServerAddr);
    if (bConnected)
    {
        TcpState.bIsConnected = true;
        TcpState.ClientSocket->SetNonBlocking(true);
        TcpState.LastReceiveTime = FPlatformTime::Seconds();
        UE_LOG(TCPClientLog, Log, TEXT("[TCP] 连接成功：%s:%d"), *IP, Port);
    }
    else
    {
        ESocketErrors Err = SocketSubsystem->GetLastErrorCode();
        UE_LOG(TCPClientLog, Error, TEXT("[TCP] 连接失败：%s"), *GetSocketErrorDescription(Err));
        DisconnectFromServer();
    }

    return bConnected;
}

void UBPFL_TcpClient::DisconnectFromServer()
{
    struct FTcpClientState& TcpState = GetTcpClientState();
    FScopeLock Lock(&TcpState.SocketCriticalSection);

    // 停止接收线程（调用上面修改后的SafeStop()）
    TcpState.bIsConnected = false;
    if (TcpState.ReceiveRunnable)
    {
        TcpState.ReceiveRunnable->SafeStop();
        delete TcpState.ReceiveRunnable;
        TcpState.ReceiveRunnable = nullptr; // 确保置空，避免二次调用
    }

    // （无需额外处理Socket，SafeStop()已彻底销毁）
    // 清空消息队列
    {
        FScopeLock QueueLock(&TcpState.MessageQueueCriticalSection);
        TcpState.MessageQueue.Empty();
    }

    TcpState.bIsAsyncReceiving = false;
    TcpState.bIsThreadRunning = false;
    UE_LOG(TCPClientLog, Log, TEXT("[TCP] 已断开连接（资源完全释放）"));
}

bool UBPFL_TcpClient::SendTCPMessage(const FString& Message)
{
    struct FTcpClientState& TcpState = GetTcpClientState();
    FScopeLock Lock(&TcpState.SocketCriticalSection);

    if (!TcpState.bIsConnected || !TcpState.ClientSocket)
    {
        UE_LOG(TCPClientLog, Error, TEXT("[TCP] 未连接，发送失败：%s"), *Message);
        return false;
    }

    // 转换消息为UTF8字节
    TArray<uint8> SendData;
    FTCHARToUTF8 Convert(*Message);
    SendData.Append((uint8*)Convert.Get(), Convert.Length());

    // 发送数据
    int32 BytesSent = 0;
    bool bSent = TcpState.ClientSocket->Send(SendData.GetData(), SendData.Num(), BytesSent);
    if (bSent && BytesSent == SendData.Num())
    {
        UE_LOG(TCPClientLog, Log, TEXT("[TCP] 发送成功：%s（%d字节）"), *Message, BytesSent);
        return true;
    }
    else
    {
        ESocketErrors Err = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode();
        UE_LOG(TCPClientLog, Error, TEXT("[TCP] 发送失败：%s（发送%d/%d字节），错误：%s"),
            *Message, BytesSent, SendData.Num(), *GetSocketErrorDescription(Err));
        return false;
    }
}

void UBPFL_TcpClient::StartAsyncReceive()
{
    struct FTcpClientState& TcpState = GetTcpClientState();
    FScopeLock Lock(&TcpState.SocketCriticalSection);

    if (!TcpState.bIsConnected || !TcpState.ClientSocket || TcpState.bIsAsyncReceiving)
    {
        UE_LOG(TCPClientLog, Warning, TEXT("[TCP] 无法启动接收（未连接/已在接收）"));
        return;
    }

    TcpState.bIsAsyncReceiving = true;
    TcpState.ReceiveRunnable = new FTCPReceiveRunnable(TcpState);
    if (TcpState.ReceiveRunnable)
    {
        TcpState.ReceiveRunnable->Start();
    }
}

bool UBPFL_TcpClient::GetQueuedMessage(FString& OutMessage)
{
    struct FTcpClientState& TcpState = GetTcpClientState();
    FScopeLock Lock(&TcpState.MessageQueueCriticalSection);

    bool bDequeued = TcpState.MessageQueue.Dequeue(OutMessage);
    if (bDequeued)
    {
        UE_LOG(TCPClientLog, Verbose, TEXT("[TCP] 取出消息：%s"), *OutMessage);
    }
    return bDequeued;
}

bool UBPFL_TcpClient::IsConnected()
{
    struct FTcpClientState& TcpState = GetTcpClientState();
    FScopeLock Lock(&TcpState.SocketCriticalSection);
    return TcpState.bIsConnected && TcpState.ClientSocket != nullptr;
}

bool UBPFL_TcpClient::TestReceiveSocketMessage(FString& OutMessage, int32& OutBytesRead)
{
    struct FTcpClientState& TcpState = GetTcpClientState();
    FScopeLock Lock(&TcpState.SocketCriticalSection);

    OutMessage.Empty();
    OutBytesRead = 0;

    if (!TcpState.bIsConnected || !TcpState.ClientSocket)
    {
        UE_LOG(TCPClientLog, Error, TEXT("[TCP] 未连接，无法接收"));
        return false;
    }

    TArray<uint8> RecvBuffer;
    RecvBuffer.SetNumUninitialized(4096);
    bool bRecvSuccess = TcpState.ClientSocket->Recv(RecvBuffer.GetData(), RecvBuffer.Num(), OutBytesRead);

    if (bRecvSuccess && OutBytesRead > 0)
    {
        OutMessage.AppendChars(reinterpret_cast<const TCHAR*>(UTF8_TO_TCHAR(RecvBuffer.GetData())), OutBytesRead);
        OutMessage = OutMessage.TrimStartAndEnd();
        TcpState.LastReceiveTime = FPlatformTime::Seconds();
        UE_LOG(TCPClientLog, Log, TEXT("[TCP] 直接接收：%s（%d字节）"), *OutMessage, OutBytesRead);
        return true;
    }
    else
    {
        ESocketErrors Err = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode();
        UE_LOG(TCPClientLog, Warning, TEXT("[TCP] 直接接收结果：%s（字节数：%d）"), *GetSocketErrorDescription(Err), OutBytesRead);
        return false;
    }
}

// 辅助函数：Socket错误码转字符串
static FString GetSocketErrorDescription(ESocketErrors ErrorCode)
{
    const int32 SE_WOULDBLOCK = 10035;
    switch (ErrorCode)
    {
    case SE_NO_ERROR:          return TEXT("无错误");
    case (ESocketErrors)SE_WOULDBLOCK: return TEXT("非阻塞模式无数据（正常）");
    case SE_ECONNRESET:        return TEXT("连接被服务器重置");
    case SE_ESHUTDOWN:         return TEXT("Socket已关闭");
    case SE_ENOTCONN:          return TEXT("未连接到服务器");
    case SE_ETIMEDOUT:         return TEXT("连接超时");
    default:                   return FString::Printf(TEXT("未知错误（码：%d）"), (int32)ErrorCode);
    }
}