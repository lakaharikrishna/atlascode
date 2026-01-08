#ifndef __PACKET_BUFFER_H__
#define __PACKET_BUFFER_H__

#include <chrono>

template <typename T>
class PacketBuffer
{
 public:
    int total_packets_received{};
    std::chrono::system_clock::time_point last_packet_time{};
    T profile_data{};

    void clear()
    {
        *this = PacketBuffer<T>{}; // Reset to default values
    }
};

#endif // __PACKET_BUFFER_H__