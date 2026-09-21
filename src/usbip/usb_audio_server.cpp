#include <winsock2.h>
#include "usb_audio_server.hpp"
#include "usb_audio_device.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <stdexcept>

namespace qm::usbip
{
namespace
{
using namespace wire;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

void receive(SOCKET socket, Bytes &data)
{
    for (size_t at = 0; at < data.size();)
    {
        const int n = recv(socket, reinterpret_cast<char *>(data.data() + at), int(data.size() - at), 0);
        if (n <= 0)
            throw std::runtime_error("USB/IP peer disconnected");
        at += n;
    }
}
void send_bytes(SOCKET socket, const Bytes &data)
{
    for (size_t at = 0; at < data.size();)
    {
        const int n =
            send(socket, reinterpret_cast<const char *>(data.data() + at), int(data.size() - at), 0);
        if (n <= 0)
            throw std::runtime_error("USB/IP response failed");
        at += n;
    }
}
struct Job
{
    unsigned sequence, direction, endpoint, size, count, start_frame;
    Bytes header, packets;
    Clock::time_point deadline;
    bool cancelled = false;
};

// reader와 스케줄러를 분리해야 긴 ISO 요청이 UNLINK/종료 처리를 가로막지 않는다.
class Connection
{
    SOCKET socket_;
    std::shared_ptr<PcmOutputBuffer> buffer_;
    std::atomic<bool> &imported_;
    std::function<void()> import_lost_;
    std::atomic<bool> stopping_ = false, done_ = false;
    std::thread reader_, scheduler_;
    std::mutex send_mutex_, jobs_mutex_;
    std::condition_variable changed_;
    std::deque<std::shared_ptr<Job>> jobs_;
    std::unordered_map<unsigned, std::shared_ptr<Job>> pending_;
    Clock::time_point epoch_ = Clock::now(), next_ = epoch_;
    std::atomic<unsigned> alternate_ = 0;
    unsigned configuration_ = 0;
    bool owns_import_ = false;

    void send_reply(unsigned command, unsigned sequence, int status, unsigned actual = 0, unsigned frame = 0,
                    unsigned count = 0xffffffff, const Bytes &data = {}, const Bytes &packets = {})
    {
        Bytes b(48);
        put(b, 0, command);
        put(b, 4, sequence);
        put(b, 20, unsigned(status));
        put(b, 24, actual);
        put(b, 28, frame);
        put(b, 32, count);
        b.insert(b.end(), data.begin(), data.end());
        b.insert(b.end(), packets.begin(), packets.end());
        std::lock_guard lock(send_mutex_);
        send_bytes(socket_, b);
    }
    void control(const Job &job)
    {
        const auto *a = job.header.data() + 40;
        const unsigned value = a[2] | unsigned(a[3]) << 8;
        const unsigned index = a[4] | unsigned(a[5]) << 8;
        const unsigned length = a[6] | unsigned(a[7]) << 8;
        Bytes data;
        int status = 0;
        if ((a[0] & 0x60) == 0)
        {
            switch (a[1])
            {
            case 6:
                data = descriptor(value >> 8, value & 255);
                if (data.empty())
                    status = -32;
                break;
            case 9:
                if (value > 1)
                    status = -32;
                else
                    configuration_ = value;
                break;
            case 8:
                data = {static_cast<unsigned char>(configuration_)};
                break;
            case 11:
                if (index != 1 || value > 1)
                {
                    status = -32;
                    break;
                }
                alternate_ = value;
                buffer_->set_consumer_active(value != 0);
                {
                    std::lock_guard lock(jobs_mutex_);
                    next_ = Clock::now();
                }
                break;
            case 10:
                data = {static_cast<unsigned char>(index == 1 ? alternate_.load() : 0)};
                break;
            case 0:
                data = {0, 0};
                break;
            case 1:
            case 3:
            case 5:
                break;
            default:
                status = -32;
            }
        }
        else if ((a[0] & 0x60) == 0x20 && (a[0] & 31) == 2 && a[1] == 0x81)
            data = {0x80, 0xbb, 0};
        else
            status = -32;
        data.resize(std::min<size_t>({data.size(), length, job.size}));
        send_reply(3, job.sequence, status, status ? 0 : unsigned(data.size()), 0, 0xffffffff,
                   status || !job.direction ? Bytes{} : data);
    }
    void schedule()
    {
        try
        {
            HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 2, TIMER_ALL_ACCESS);
            if (!timer)
                throw std::runtime_error("High resolution audio timer unavailable");
            struct Close
            {
                HANDLE h;
                ~Close()
                {
                    CloseHandle(h);
                }
            } close{timer};
            for (;;)
            {
                std::unique_lock lock(jobs_mutex_);
                changed_.wait(lock,
                              [&]
                              {
                                  return stopping_ || !jobs_.empty();
                              });
                if (stopping_)
                    return;
                auto job = jobs_.front();
                jobs_.pop_front();
                lock.unlock();
                // 최대 2ms마다 취소/종료를 확인한다. 실제 대기는 고해상도 타이머로 하여
                // 일반 sleep의 해상도가 오디오 시계에 누적되지 않도록 한다.
                for (;;)
                {
                    lock.lock();
                    const bool cancelled = job->cancelled || stopping_;
                    lock.unlock();
                    const auto remaining = job->deadline - Clock::now();
                    if (cancelled || remaining <= Clock::duration::zero())
                        break;
                    LARGE_INTEGER due;
                    due.QuadPart = -std::max<int64_t>(
                        1,
                        std::min<int64_t>(
                            20000,
                            std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100));
                    if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) ||
                        WaitForSingleObject(timer, 1000) != WAIT_OBJECT_0)
                        throw std::runtime_error("USB audio pacing failed");
                }
                lock.lock();
                if (stopping_)
                    return;
                if (job->cancelled)
                    continue;
                // 잠금 아래에서 완료를 확정한다. UNLINK와 동시에 들어와도 SUBMIT 완료 또는
                // 취소 중 하나만 성공하며, 동일 요청을 두 번 완료하지 않는다.
                Bytes data;
                data.reserve(job->count * 96);
                unsigned actual = 0;
                for (unsigned i = 0; i < job->count; ++i)
                {
                    const auto bytes =
                        alternate_ ? std::min(number(job->packets.data() + i * 16 + 4), 96u) : 0;
                    std::array<int16_t, 48> pcm{};
                    buffer_->read(std::span<int16_t>(pcm.data(), bytes / 2));
                    for (unsigned j = 0; j < bytes / 2; ++j)
                    {
                        data.push_back(pcm[j] & 255);
                        data.push_back((uint16_t(pcm[j]) >> 8) & 255);
                    }
                    put(job->packets, i * 16 + 8, bytes);
                    put(job->packets, i * 16 + 12, 0);
                    actual += bytes;
                }
                send_reply(3, job->sequence, 0, actual, job->start_frame, job->count, data, job->packets);
                pending_.erase(job->sequence);
            }
        }
        catch (...)
        {
            stop();
        }
    }
    void run()
    {
        try
        {
            Bytes h(8);
            receive(socket_, h);
            if (h[0] != 1 || h[1] != 0x11 || number(h.data() + 4))
                throw std::runtime_error("Invalid USB/IP operation");
            const unsigned op = unsigned(h[2]) << 8 | h[3];
            if (op == 0x8005)
            {
                auto response = operation(5);
                response.resize(12);
                put(response, 8, 1);
                auto device = description();
                response.insert(response.end(), device.begin(), device.end());
                response.insert(response.end(), {1, 1, 0, 0, 1, 2, 0, 0});
                send_bytes(socket_, response);
            }
            else if (op == 0x8003)
            {
                Bytes bus(32);
                receive(socket_, bus);
                if (bus[0] != '1' || bus[1] != '-' || bus[2] != '1' ||
                    std::any_of(bus.begin() + 3, bus.end(),
                                [](auto c)
                                {
                                    return c != 0;
                                }))
                    send_bytes(socket_, operation(3, 1));
                else if (imported_.exchange(true))
                    send_bytes(socket_, operation(3, 2));
                else
                {
                    owns_import_ = true;
                    send_bytes(socket_, operation(3));
                    send_bytes(socket_, description());
                    // 열거가 끝난 가져오기 연결은 오디오를 사용하지 않는 동안에도 유지된다.
                    DWORD infinite = 0;
                    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&infinite),
                               sizeof(infinite));
                    scheduler_ = std::thread(
                        [this]
                        {
                            schedule();
                        });
                    read_requests();
                }
            }
        }
        catch (...)
        { /* 잘못된 요청이나 연결 해제는 해당 연결만 종료한다. */
        }
        stop();
        if (scheduler_.joinable())
            scheduler_.join();
        if (owns_import_)
        {
            buffer_->set_consumer_active(false);
            imported_ = false;
            if (import_lost_)
                import_lost_();
        }
        done_ = true;
    }
    void read_requests()
    {
        while (!stopping_)
        {
            Bytes h(48);
            receive(socket_, h);
            const unsigned command = number(h.data()), sequence = number(h.data() + 4);
            if (command == 2)
            {
                std::lock_guard lock(jobs_mutex_);
                const auto found = pending_.find(number(h.data() + 20));
                const bool cancelled = found != pending_.end();
                if (cancelled)
                {
                    found->second->cancelled = true;
                    pending_.erase(found);
                }
                send_reply(4, sequence, cancelled ? -104 : 0);
                continue;
            }
            if (command != 1 || number(h.data() + 8) != device_id)
                throw std::runtime_error("Invalid USB request");
            auto job = std::make_shared<Job>();
            job->header = std::move(h);
            job->sequence = sequence;
            job->direction = number(job->header.data() + 12);
            job->endpoint = number(job->header.data() + 16);
            job->size = number(job->header.data() + 24);
            job->count = number(job->header.data() + 32);
            const bool iso = job->count && job->count != 0xffffffff;
            if (job->direction > 1 || job->size > max_payload || (iso && job->count > max_iso_packets))
                throw std::runtime_error("USB request exceeds bounds");
            Bytes output(job->direction ? 0 : job->size);
            receive(socket_, output);
            job->packets.resize(iso ? job->count * 16 : 0);
            receive(socket_, job->packets);
            if (job->endpoint == 0 && !iso)
            {
                control(*job);
                continue;
            }
            if (job->endpoint != 1 || job->direction != 1 || !iso)
            {
                send_reply(3, sequence, -32);
                continue;
            }
            for (unsigned i = 0; i < job->count; ++i)
            {
                const auto offset = number(job->packets.data() + i * 16);
                const auto length = number(job->packets.data() + i * 16 + 4);
                if (offset > job->size || length > job->size - offset || length % 2)
                    throw std::runtime_error("Invalid ISO packet range");
            }
            std::lock_guard lock(jobs_mutex_);
            if (pending_.size() >= 64 || jobs_.size() >= 64 || pending_.contains(sequence))
                throw std::runtime_error("USB request queue exceeds bounds");
            if (Clock::now() - next_ > 100ms)
                next_ = Clock::now();
            job->start_frame =
                unsigned(std::chrono::duration_cast<std::chrono::milliseconds>(next_ - epoch_).count()) &
                0x7ff;
            next_ += std::chrono::milliseconds(job->count);
            job->deadline = next_;
            pending_.emplace(sequence, job);
            jobs_.push_back(job);
            changed_.notify_one();
        }
    }

  public:
    Connection(SOCKET socket, std::shared_ptr<PcmOutputBuffer> buffer, std::atomic<bool> &imported,
               std::function<void()> import_lost)
        : socket_(socket), buffer_(std::move(buffer)), imported_(imported),
          import_lost_(std::move(import_lost))
    {
        DWORD timeout = 2000;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout),
                   sizeof(timeout));
        setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout),
                   sizeof(timeout));
        reader_ = std::thread(
            [this]
            {
                run();
            });
    }
    ~Connection()
    {
        stop();
        if (reader_.joinable())
            reader_.join();
        closesocket(socket_);
    }
    void stop()
    {
        // 전송이 큐 잠금을 보유한 경우에도 소켓을 먼저 깨워 정리가 지연되지 않게 한다.
        shutdown(socket_, SD_BOTH);
        {
            // 스케줄러의 조건 확인과 대기 진입 사이에 종료 알림이 사라지지 않게 한다.
            std::lock_guard lock(jobs_mutex_);
            stopping_ = true;
        }
        changed_.notify_all();
    }
    bool done() const
    {
        return done_;
    }
};
} // namespace

struct UsbAudioServer::Impl
{
    std::shared_ptr<PcmOutputBuffer> buffer;
    SOCKET listener = INVALID_SOCKET;
    unsigned short port = 0;
    std::atomic<bool> stopping = false, imported = false;
    std::thread accept_thread;
    std::vector<std::unique_ptr<Connection>> connections;
    std::mutex callback_mutex;
    std::function<void()> import_lost;
    explicit Impl(std::shared_ptr<PcmOutputBuffer> value) : buffer(std::move(value))
    {
        if (!buffer)
            throw std::invalid_argument("USB audio buffer required");
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data))
            throw std::runtime_error("Winsock initialization failed");
    }
    ~Impl()
    {
        WSACleanup();
    }
};
UsbAudioServer::UsbAudioServer(std::shared_ptr<PcmOutputBuffer> buffer)
    : impl_(std::make_unique<Impl>(std::move(buffer)))
{
}
UsbAudioServer::~UsbAudioServer()
{
    stop();
}
void UsbAudioServer::start()
{
    stop();
    auto &s = *impl_;
    s.stopping = false;
    s.listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s.listener == INVALID_SOCKET)
        throw std::runtime_error("USB/IP listener creation failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int length = sizeof(address);
    if (bind(s.listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) || listen(s.listener, 4) ||
        getsockname(s.listener, reinterpret_cast<sockaddr *>(&address), &length))
    {
        stop();
        throw std::runtime_error("USB/IP localhost listener unavailable");
    }
    s.port = ntohs(address.sin_port);
    try
    {
        // 최대 연결 수만큼 미리 확보한다. 연결 객체가 소켓을 인수한 뒤 벡터 할당 실패로
        // 객체와 catch 양쪽에서 같은 소켓을 닫는 상황을 방지한다.
        s.connections.reserve(4);
        s.accept_thread = std::thread(
            [&s]
            {
                while (!s.stopping)
                {
                    fd_set ready;
                    FD_ZERO(&ready);
                    FD_SET(s.listener, &ready);
                    timeval timeout{0, 100000};
                    if (select(0, &ready, nullptr, nullptr, &timeout) <= 0)
                        continue;
                    SOCKET peer = accept(s.listener, nullptr, nullptr);
                    if (peer == INVALID_SOCKET)
                        continue;
                    std::erase_if(s.connections,
                                  [](const auto &connection)
                                  {
                                      return connection->done();
                                  });
                    if (s.connections.size() >= 4)
                    {
                        closesocket(peer);
                        continue;
                    }
                    try
                    {
                        s.connections.push_back(
                            std::make_unique<Connection>(peer, s.buffer, s.imported,
                                                         [&s]
                                                         {
                                                             std::function<void()> handler;
                                                             {
                                                                 std::lock_guard lock(s.callback_mutex);
                                                                 if (!s.stopping)
                                                                     handler = s.import_lost;
                                                             }
                                                             // 종료와 콜백 수명은 stop의 연결 스레드 join으로 보장한다.
                                                             if (handler)
                                                                 handler();
                                                         }));
                    }
                    catch (...)
                    {
                        closesocket(peer);
                    }
                }
            });
    }
    catch (...)
    {
        stop();
        throw;
    }
}
void UsbAudioServer::stop()
{
    auto &s = *impl_;
    s.stopping = true;
    if (s.accept_thread.joinable())
        s.accept_thread.join();
    for (auto &connection : s.connections)
        connection->stop();
    s.connections.clear();
    if (s.listener != INVALID_SOCKET)
    {
        closesocket(s.listener);
        s.listener = INVALID_SOCKET;
    }
    s.port = 0;
    s.buffer->set_consumer_active(false);
    s.buffer->reset();
}
unsigned short UsbAudioServer::port() const
{
    return impl_->port;
}
void UsbAudioServer::set_import_lost_handler(std::function<void()> handler)
{
    std::lock_guard lock(impl_->callback_mutex);
    impl_->import_lost = std::move(handler);
}
} // namespace qm::usbip
