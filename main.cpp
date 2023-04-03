#include <iostream>
#include <exception>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef __linux__
#include <arpa/inet.h>
#include <sys/socket.h>
#elif defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#include <windows.h>
#include <winsock2.h>
#include <WS2tcpip.h>
#include <fileapi.h>

#else

#endif

#include "udpbd.h"

#define BUFLEN 2048

using namespace std;

/*
 * class CBlockDevice
 */
class CBlockDevice
{
public:
    CBlockDevice(const char *sFileName) : _read_only(false)
    {
        // Open the selected file.
        // This file will be used as the Block Device
        _fp = CreateFile(sFileName, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);

        if (_fp == INVALID_HANDLE_VALUE)
        {
            _read_only = true;

            //_fp = open(sFileName, _read_only ? O_RDONLY : O_RDWR);
            _fp = CreateFile(sFileName, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
            if (_fp == INVALID_HANDLE_VALUE)
                throw runtime_error(string("unable to open file ") + sFileName);
        }

        DWORD status;
        // lock volume
        if (!DeviceIoControl(_fp, FSCTL_LOCK_VOLUME,
                             NULL, 0, NULL, 0, &status, NULL))
        {
            printf("Error %d attempting to lock device\n", GetLastError());
        }

        if (!DeviceIoControl(_fp, FSCTL_DISMOUNT_VOLUME,
                             NULL, 0, NULL, 0, &status, NULL))
        {
            DWORD err = GetLastError();
            printf("Error %d attempting to dismount volume, error code\n", err);
        }

        // Get the size of the file
        DWORD junk = 0;
        DISK_GEOMETRY pdg;
        BOOL bResult = DeviceIoControl(_fp,                           // device to be queried
                                       IOCTL_DISK_GET_DRIVE_GEOMETRY, // operation to perform
                                       NULL, 0,                       // no input buffer
                                       &pdg, sizeof(pdg),             // output buffer
                                       &junk,                         // # bytes returned
                                       (LPOVERLAPPED)NULL);

        if (bResult)
        {
            sector_offset = 0;
            _fsize = pdg.Cylinders.QuadPart * (ULONG)pdg.TracksPerCylinder *
                     (ULONG)pdg.SectorsPerTrack * (ULONG)pdg.BytesPerSector;
            sector_size = pdg.BytesPerSector;
            printf("Opened '%s' as Block Device\n", sFileName);
            printf(" - %s\n", _read_only ? "read-only" : "read/write");
            printf(" - size = %ldMB / %ldMiB, sector size = %ld\n", _fsize / (1000 * 1000), _fsize / (1024 * 1024), sector_size);
        }
        else
            printf("Error: %d\n", GetLastError());
        fflush(stdout);
    }

    ~CBlockDevice()
    {
        CloseHandle(_fp);
    }

    void seek(uint32_t sector)
    {
        _off64_t offset = (_off64_t)sector * sector_size;
        LONG high, low;
        low = (offset);
        high = (offset >> 32);
        // printf("seek %d * 512 = %ld\n", sector, offset);
        SetFilePointer(_fp, low, &high, FILE_BEGIN);
    }

    void read(void *data, size_t size)
    {

        size_t new_size, remainder, aux_size;
        DWORD rv;

        // Size is re-computed as factor of sector size
        new_size = (size - sector_offset);
        remainder = new_size % sector_size;
        aux_size = sector_size * (new_size / sector_size + (remainder == 0 ? 0 : 1));

        BOOL ret = ReadFile(_fp, (char *)data + sector_offset, aux_size, &rv, NULL);

        if (ret == true)
        {
            // If previous call had remaining bytes, use them in this call
            if (sector_offset)
            {
                memcpy(data, sector_buffer, sector_offset);
            }

            // If this call has remaining bytes, store them for next call
            if (remainder != 0)
            {
                memcpy(sector_buffer, data + size, sector_size - remainder);
                sector_offset = (sector_size - remainder);
            }
            else
                sector_offset = 0;
        }
        else
            printf("Error reading sectors: %ld\n", GetLastError());
    }

    // TODO: This method is not optimized, nonetheless in game write operations are not critical
    void write(const void *data, size_t size)
    {
        DWORD rv;
        LONG high, low;

        // Size is re-computed as factor of sector size
        size_t aux_size = sector_size * (size / sector_size + ((size % sector_size) != 0 ? 1 : 0));

        // First read sectors
        low = SetFilePointer(_fp, 0, &high, FILE_CURRENT);
        BOOL ret = ReadFile(_fp, sector_buffer, aux_size, &rv, NULL);

        // Then inject received data
        memcpy(sector_buffer, data, size);

        // Finally write sectors
        SetFilePointer(_fp, low, &high, FILE_BEGIN);
        ret = WriteFile(_fp, sector_buffer, aux_size, &rv, NULL);
        // printf("write %ld\n", size);
        if (ret == 0)
            printf("write error %ld != %ld,%d\n", rv, size, GetLastError());
    }

    uint32_t get_sector_size() { return sector_size; }
    uint32_t get_sector_count() { return _fsize / sector_size; }
    void clear_sector_offset(){sector_offset=0;}

private:
    bool _read_only;
    HANDLE _fp;
    size_t sector_size;
    size_t sector_offset;
    uint8_t sector_buffer[4 * 512];
    _off64_t _fsize;
};

/*
 * class CUDPBDServer
 */
class CUDPBDServer
{
public:
    CUDPBDServer(class CBlockDevice &bd) : _bd(bd), _block_shift(0)
    {
        set_block_shift(5); // 128b blocks
        struct sockaddr_in si_me;

        WORD wVersionRequested;
        WSADATA wsaData;
        int err;

        /* Use the MAKEWORD(lowbyte, highbyte) macro declared in Windef.h */
        wVersionRequested = MAKEWORD(2, 2);

        err = WSAStartup(wVersionRequested, &wsaData);
        if (err != 0)
        {
            /* Tell the user that we could not find a usable */
            /* Winsock DLL.                                  */
            printf("WSAStartup failed with error: %d\n", err);
        }

        // create a UDP socket
        if ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
        {
            printf("%d\n", GetLastError());
            throw runtime_error("socket");
        }

        // bind socket to port
        memset((char *)&si_me, 0, sizeof(si_me));
        si_me.sin_family = AF_INET;
        si_me.sin_port = htons(UDPBD_PORT);
        si_me.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, (struct sockaddr *)&si_me, sizeof(si_me)) == -1)
        {
            throw runtime_error("bind");
        }

        // Enable broadcasts
        int broadcastEnable = 1;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char *)&broadcastEnable, sizeof(broadcastEnable));
    }

    ~CUDPBDServer()
    {
        close(s);
    }

    void run()
    {
        struct sockaddr_in si_other;
        socklen_t slen = sizeof(si_other);
        int recv_len;
        char buf[BUFLEN];

        printf("Server running on port %d (0x%x)\n", UDPBD_PORT, UDPBD_PORT);

        // Start server loop
        while (1)
        {
            // Receive command from ps2
            if ((recv_len = recvfrom(s, buf, BUFLEN, 0, (struct sockaddr *)&si_other, &slen)) == -1)
            {
                throw runtime_error("recvfrom");
            }

            struct SUDPBDv2_Header *hdr = (struct SUDPBDv2_Header *)buf;

            // Process command
            switch (hdr->cmd)
            {
            case UDPBD_CMD_INFO:
                handle_cmd_info(si_other, (struct SUDPBDv2_InfoRequest *)buf);
                break;
            case UDPBD_CMD_READ:
                handle_cmd_read(si_other, (struct SUDPBDv2_RWRequest *)buf);
                break;
            case UDPBD_CMD_WRITE:
                handle_cmd_write(si_other, (struct SUDPBDv2_RWRequest *)buf);
                break;
            case UDPBD_CMD_WRITE_RDMA:
                handle_cmd_write_rdma(si_other, (struct SUDPBDv2_RDMA *)buf);
                break;
            default:
                printf("Invalid cmd: 0x%x\n", hdr->cmd);
            };
        }
    }

private:
    void set_block_shift(uint32_t shift)
    {
        if (shift != _block_shift)
        {
            _block_shift = shift;
            _block_size = 1 << (_block_shift + 2);
            _blocks_per_packet = RDMA_MAX_PAYLOAD / _block_size;
            _blocks_per_sector = _bd.get_sector_size() / _block_size;
            printf("Block size changed to %d\n", _block_size);
        }
    }

    void set_block_shift_sectors(uint32_t sectors)
    {
        // Optimize for:
        // 1 - the least number of network packets
        // 2 - the largest block size (faster on the ps2)
        uint32_t shift;
        uint32_t size = sectors * 512;
        uint32_t packetsMIN = (size + 1440 - 1) / 1440;
        uint32_t packets128 = (size + 1408 - 1) / 1408;
        uint32_t packets256 = (size + 1280 - 1) / 1280;
        uint32_t packets512 = (size + 1024 - 1) / 1024;

        if (packets512 == packetsMIN)
            shift = 7; // 512 byte blocks
        else if (packets256 == packetsMIN)
            shift = 6; // 256 byte blocks
        else if (packets128 == packetsMIN)
            shift = 5; // 128 byte blocks
        else
            shift = 3; //  32 byte blocks

        set_block_shift(shift);
    }

    void handle_cmd_info(struct sockaddr_in &si_other, struct SUDPBDv2_InfoRequest *request)
    {
        struct SUDPBDv2_InfoReply reply;

        char str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &si_other.sin_addr, str, INET_ADDRSTRLEN);

        printf("UDPBD_CMD_INFO from %s\n", str);

        // Reply header
        reply.hdr.cmd = UDPBD_CMD_INFO_REPLY;
        reply.hdr.cmdid = request->hdr.cmdid;
        reply.hdr.cmdpkt = 1;
        // Reply info
        reply.sector_size = _bd.get_sector_size();
        reply.sector_count = _bd.get_sector_count();

        // Send packet to ps2
        if (sendto(s, (char *)&reply, sizeof(reply), 0, (struct sockaddr *)&si_other, sizeof(si_other)) == -1)
        {
            throw runtime_error("sendto");
        }
    }

    void handle_cmd_read(struct sockaddr_in &si_other, struct SUDPBDv2_RWRequest *request)
    {
        struct SUDPBDv2_RDMA reply;

        // printf("UDPBD_CMD_READ(cmdId=%d, startSector=%d, sectorCount=%d)\n", request->hdr.cmdid, request->sector_nr, request->sector_count);

        // Optimize RDMA block size for number of sectors
        set_block_shift_sectors(request->sector_count);

        // Reply header
        reply.hdr.cmd = UDPBD_CMD_READ_RDMA;
        reply.hdr.cmdid = request->hdr.cmdid;
        reply.hdr.cmdpkt = 1;
        reply.bt.block_shift = _block_shift;

        uint32_t blocks_left = request->sector_count * _blocks_per_sector;

        _bd.seek(request->sector_nr);
        _bd.clear_sector_offset();

        // Packet loop
        while (blocks_left > 0)
        {
            reply.bt.block_count = (blocks_left > _blocks_per_packet) ? _blocks_per_packet : blocks_left;
            blocks_left -= reply.bt.block_count;

            // read data from file
            _bd.read(reply.data2, reply.bt.block_count * _block_size);

            // Send packet to ps2
            if (sendto(s, (char *)&reply, sizeof(struct SUDPBDv2_Header) + 4 + (reply.bt.block_count * _block_size), 0, (struct sockaddr *)&si_other, sizeof(si_other)) == -1)
            {
                throw runtime_error("sendto");
            }
            reply.hdr.cmdpkt++;
        }
    }

    void handle_cmd_write(struct sockaddr_in &si_other, struct SUDPBDv2_RWRequest *request)
    {
        printf("UDPBD_CMD_WRITE(cmdId=%d, startSector=%d, sectorCount=%d)\n", request->hdr.cmdid, request->sector_nr, request->sector_count);

        _bd.seek(request->sector_nr);
        _write_size_left = request->sector_count * 512;
    }

    void handle_cmd_write_rdma(struct sockaddr_in &si_other, struct SUDPBDv2_RDMA *request)
    {
        size_t size = request->bt.block_count * (1 << (request->bt.block_shift + 2));
        // printf("UDPBD_CMD_WRITE_RDMA(cmdId=%d, BS=%d, BC=%d, size=%ld)\n", request->hdr.cmdid, request->bt.block_shift, request->bt.block_count, size);

        _bd.write(request->data2, size);
        _write_size_left -= size;
        if (_write_size_left == 0)
        {
            struct SUDPBDv2_WriteDone reply;

            // Reply header
            reply.hdr.cmd = UDPBD_CMD_WRITE_DONE;
            reply.hdr.cmdid = request->hdr.cmdid;
            reply.hdr.cmdpkt = request->hdr.cmdid + 1;
            reply.result = 0;

            // Send packet to ps2
            if (sendto(s, (char *)&reply, sizeof(reply), 0, (struct sockaddr *)&si_other, sizeof(si_other)) == -1)
            {
                throw runtime_error("sendto");
            }
        }
    }

    class CBlockDevice &_bd;
    uint32_t _block_shift;
    uint32_t _block_size;
    uint32_t _blocks_per_packet;
    uint32_t _blocks_per_sector;
    int s;

    uint32_t _write_size_left;
};

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        printf("Usage:\n");
        printf("  %s <file>\n", argv[0]);
        return -1;
    }

    try
    {
        class CBlockDevice bd(argv[1]);
        class CUDPBDServer srv(bd);
        srv.run();
    }
    catch (exception &e)
    {
        cout << e.what() << '\n';
        return -2;
    }

    return 0;
}
