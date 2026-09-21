// 앱과 커널 드라이버가 함께 읽는 전송 계약이다. 패킷 배치와 상수를 공유하여
// 서로 다른 모듈이 같은 바이트를 같은 의미로 해석하게 한다.
#pragma once
// 커널 빌드에서는 일반 사용자 모드 헤더 대신 필요한 고정 크기 정수 별칭만 제공한다.
#ifdef _KERNEL_MODE
using uint32_t = unsigned int;
using int16_t = short;
#else
#include <stdint.h>
#endif
namespace qm::driver
{
inline unsigned long long packet_start_position(unsigned long long completed, uint32_t packet_bytes)
{
    return completed ? (completed - 1) * packet_bytes : 0;
}
// 실제 오디오 위치와 1ms 미만의 시간 잔여분을 기준으로 다음 패킷 경계를 예약한다.
// 매번 고정 주기를 더하면 타이머 지연이 누적되어 캡처 패킷을 건너뛰게 된다.
inline unsigned long long next_packet_delay_hns(unsigned long long position, unsigned long long carry_hns,
                                                uint32_t packet_bytes, uint32_t bytes_per_second)
{
    if (!packet_bytes || !bytes_per_second)
        return 10000;
    const auto remaining = (packet_bytes - position % packet_bytes) * 10000000 / bytes_per_second;
    return remaining > carry_hns ? remaining - carry_hns : 1;
}
// 버퍼 바이트 수를 알림 횟수로 나누어 알림 간격(ms)을 계산한다.
// 48 kHz × 모노 × 2바이트 = 1 ms당 96바이트이므로 정수 ms로 나누어떨어질 때만 허용한다.
inline uint32_t notification_interval_ms(uint32_t bytes, uint32_t notifications)
{
    // Mono PCM16: 96 bytes/ms. Reject unsupported sub-ms/fractional periods.
    if (bytes == 0 || bytes > 96000 || notifications == 0 || notifications > 2 || bytes % notifications)
        return 0;
    // 먼저 전체 길이와 알림 수를 검증했으므로 0으로 나누지 않는다. 0 반환은 유효한 간격이 아니라
    // 지원하지 않는 요청이라는 표시다.
    const auto packet_bytes = bytes / notifications;
    return packet_bytes % 96 ? 0 : packet_bytes / 96;
}
// 480샘플은 48,000샘플/초에서 10 ms이다. 버전은 수신 측이 다른 구조의 패킷을 거르기 위해 쓴다.
inline constexpr uint32_t version = 1, sample_rate = 48000, frame_samples = 480;
inline constexpr wchar_t device_path[] = L"\\\\.\\QuietMicStream";
inline constexpr wchar_t hardware_id[] = L"ROOT\\QuietMic";
// METHOD_BUFFERED, FILE_WRITE_ACCESS, private device type 0x8000.
// IOCTL 숫자는 장치 종류, 접근 권한, 기능 번호, 전달 방식을 비트로 합친 값이다.
// write는 음성을 보내고 reset은 이미 대기 중인 음성을 비우는 별도 요청이다.
inline constexpr uint32_t write_ioctl = (0x8000u << 16) | (2u << 14) | (0x800u << 2);
inline constexpr uint32_t reset_ioctl = (0x8000u << 16) | (2u << 14) | (0x801u << 2);
// count는 유효한 샘플 수이며 배열 자체는 항상 480칸이다. 가변 길이 메모리가 아니라
// 이 고정 구조체 전체를 보내므로 수신 측은 바이트 길이와 count를 모두 확인한다.
struct Packet
{
    uint32_t protocol_version;
    uint32_t count;
    int16_t samples[frame_samples];
};
// queued_samples는 아직 읽지 않은 샘플 수, underruns/overruns는 부족/초과가 발생한 횟수다.
// 밀리초나 누적 손실 샘플 수가 아니므로 UI에서 해석할 때 단위를 구별한다.
struct Status
{
    uint32_t protocol_version, queued_samples, underruns, overruns;
};
// 컴파일 단계에서 앱/드라이버가 합의한 구조체 크기를 확인하여 패딩 변화도 감지한다.
static_assert(sizeof(Packet) == 968);
static_assert(sizeof(Status) == 16);
// 실제 전달된 바이트 수, 프로토콜 버전, 유효 샘플 수가 모두 맞아야 배열에 접근한다.
inline bool valid_packet(const Packet &p, uint32_t bytes)
{
    return bytes == sizeof(Packet) && p.protocol_version == version && p.count > 0 &&
           p.count <= frame_samples;
}
// Single producer/consumer, synchronized by the kernel caller's spin lock.
// 고정 크기 원형 큐다. head_는 다음에 읽을 위치, size_는 대기 샘플 수이며
// 배열 끝을 넘는 인덱스는 나머지 연산으로 처음으로 돌아온다. 내부에는 잠금이 없어서
// 커널 사용자는 스핀 잠금으로 보호하고, 앱의 PreviewBuffer는 한 작업 스레드만 접근한다.
class Queue
{
    // USB 가상 마이크만 Windows 스케줄링 버스트를 위해 60ms를 사용한다.
    // 기본 저지연 큐와 스피커 미리듣기는 기존 40ms 상한을 유지한다.
    static constexpr uint32_t storage_capacity = 2880;
    static constexpr uint32_t low_latency_capacity = 1920;
    int16_t samples_[storage_capacity]{};
    uint32_t head_ = 0, size_ = 0, under_ = 0, over_ = 0;
    uint32_t capacity_ = low_latency_capacity;
    bool active_ = false;
    bool buffered_ = false, ready_ = true;

  public:
    // 가상 드라이버는 외부 소비자의 시계를 멈출 수 없어 큐 자체에서 시작 여유를 확보한다.
    // 스피커 미리 듣기는 별도로 WASAPI 시작을 지연하므로 기본 정책을 그대로 사용한다.
    constexpr explicit Queue(bool buffered = false)
        : capacity_(buffered ? storage_capacity : low_latency_capacity), buffered_(buffered),
          ready_(!buffered)
    {
    }
    // 음소거에서는 대기 음성만 버리고 세션 오류 기록을 보존한다.
    // 새 연결/캡처 세션에서는 기본값으로 통계까지 초기화한다.
    void reset(bool clear_counters = true)
    {
        head_ = size_ = 0;
        ready_ = !buffered_;
        if (clear_counters)
            under_ = over_ = 0;
    }
    // 캡처를 열거나 닫는 경계에서 이전 음성이 새 세션으로 넘어가지 않도록 초기화한다.
    void capture_active(bool value)
    {
        if (value != active_)
        {
            reset();
            active_ = value;
        }
    }
    bool write(const Packet &p, uint32_t bytes)
    {
        if (!valid_packet(p, bytes))
            return false;
        // 듣는 클라이언트가 없으면 성공으로 응답하되 저장하지 않는다. 나중에 연결된 앱이
        // 과거의 마이크 음성을 재생하는 것을 막는 정책이다.
        if (!active_)
            return true; // No client: discard speech rather than replay it later.
        // 버퍼가 가득 차면 가장 오래된 샘플부터 필요한 만큼 버린다. 새 음성을 살려
        // 지연이 40 ms를 넘어서 계속 증가하지 않게 하고 초과 사건을 한 번 기록한다.
        if (size_ + p.count > capacity_)
        {
            uint32_t drop = size_ + p.count - capacity_;
            head_ = (head_ + drop) % capacity_;
            size_ -= drop;
            ++over_;
        }
        // 현재 꼬리(head_ + size_)부터 새 패킷을 이어 붙인 뒤 유효 길이를 늘린다.
        for (uint32_t i = 0; i < p.count; i++)
            samples_[(head_ + size_ + i) % capacity_] = p.samples[i];
        size_ += p.count;
        return true;
    }
    // 요청한 길이는 항상 채운다. 부족한 꼬리는 0(무음)으로 메우고, 이 읽기 요청의
    // 부족 사건은 샘플마다가 아니라 한 번만 센다.
    void read(int16_t *out, uint32_t count)
    {
        // 처음과 재시작 때는 30ms를 모은다. 첫 10ms 소비 후에도 20ms 여유를 남겨
        // 생산자의 10ms 프레임이 11~12ms 뒤 도착하는 시간 흔들림을 흡수한다.
        // 기다리는 동안 의도적인 무음을 출력하며
        // 이미 도착한 음성을 소모하지 않아 캡처/소비 이벤트의 시간차를 흡수한다.
        if (!ready_)
        {
            ready_ = size_ >= 3 * frame_samples;
            if (!ready_)
            {
                for (uint32_t i = 0; i < count; ++i)
                    out[i] = 0;
                return;
            }
        }
        if (size_ < count)
        {
            ++under_;
            // 실제 전달이 시작된 뒤의 부족은 기록한다. 다음 읽기부터 여유를 다시 확보한다.
            if (buffered_)
                ready_ = false;
        }
        for (uint32_t i = 0; i < count; i++)
        {
            out[i] = size_ ? samples_[head_] : 0;
            if (size_)
            {
                head_ = (head_ + 1) % capacity_;
                --size_;
            }
        }
    }
    // 현재 통계를 값으로 복사한다. 동시 접근에서 일관된 값을 얻는 책임도 호출자 잠금에 있다.
    Status status() const
    {
        return {version, size_, under_, over_};
    }
};
} // namespace qm::driver
