#include <iostream>
#include <exception>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "udpbd.h"

#define BUFLEN  2048


using namespace std;


/*
 * class CBlockDevice
 */
class CBlockDevice
{
public:
    CBlockDevice(const char *sFileName) : _read_only(false) {
        // Open the selected file.
        // This file will be used as the Block Device
        _fp = open(sFileName, _read_only ? O_RDONLY : O_RDWR);
        if (_fp < 0) {
            _read_only = true;

            _fp = open(sFileName, _read_only ? O_RDONLY : O_RDWR);
            if (_fp < 0)
                throw runtime_error(string("unable to open file ") + sFileName);
        }

        // Get the size of the file
        _fsize = lseek64(_fp, 0, SEEK_END);
        lseek64(_fp, 0, SEEK_SET);

        printf("Opened '%s' as Block Device\n", sFileName);
        printf(" - %s\n", _read_only ? "read-only" : "read/write");
        printf(" - size = %ldMB / %ldMiB\n", _fsize / (1000*1000), _fsize / (1024*1024));
    }

    ~CBlockDevice() {
        close(_fp);
    }

    void seek(uint32_t sector) {
        loff_t offset = (loff_t)sector * 512;
        //printf("seek %d * 512 = %ld\n", sector, offset);
        lseek64(_fp, offset, SEEK_SET);
    }

    void read(void *data, size_t size) {
        ssize_t rv = ::read(_fp, data, size);
        if (rv != size)
            printf("read error %ld != %ld\n", rv, size);
    }

    void write(const void *data, size_t size) {
        ssize_t rv = ::write(_fp, data, size);
        //printf("write %ld\n", size);
        if (rv != size)
            printf("write error %ld != %ld\n", rv, size);
    }

    uint32_t get_sector_size()  {return 512;}
    uint32_t get_sector_count() {return _fsize/512;}

private:
    int _fp;
    bool _read_only;
    loff_t _fsize;
};

/*
 * class CUDPBDServer
 */
class CUDPBDServer
{
public:
    CUDPBDServer(class CBlockDevice &bd) : _bd(bd), _block_shift(0) {
        set_block_shift(5); // 128b blocks
        struct sockaddr_in si_me;

        //create a UDP socket
        if ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
            throw runtime_error("socket");
        }

        //bind socket to port
        memset((char *) &si_me, 0, sizeof(si_me));
        si_me.sin_family = AF_INET;
        si_me.sin_port = htons(UDPBD_PORT);
        si_me.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, (struct sockaddr*)&si_me, sizeof(si_me) ) == -1) {
            throw runtime_error("bind");
        }

        // Enable broadcasts
        int broadcastEnable=1;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable));
    }

    ~CUDPBDServer() {
        close(s);
    }

    void run() {
        struct sockaddr_in si_other;
        socklen_t slen = sizeof(si_other);
        int recv_len;
        char buf[BUFLEN];

        printf("Server running on port %d (0x%x)\n", UDPBD_PORT, UDPBD_PORT);

        // Start server loop
        while (1) {
            // Receive command from ps2
            if ((recv_len = recvfrom(s, buf, BUFLEN, 0, (struct sockaddr *) &si_other, &slen)) == -1) {
                throw runtime_error("recvfrom");
            }

            struct SUDPBDv2_Header *hdr = (struct SUDPBDv2_Header *)buf;

            // Process command
            switch (hdr->cmd) {
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
    void set_block_shift(uint32_t shift) {
        if (shift != _block_shift) {
            _block_shift       = shift;
            _block_size        = 1 << (_block_shift + 2);
            _blocks_per_packet = RDMA_MAX_PAYLOAD / _block_size;
            _blocks_per_sector = _bd.get_sector_size() / _block_size;
            printf("Block size changed to %d\n", _block_size);
        }
    }

    void set_block_shift_sectors(uint32_t sectors) {
        // Optimize for:
        // 1 - the least number of network packets
        // 2 - the largest block size (faster on the ps2)
        uint32_t shift;
        uint32_t size = sectors * 512;
        uint32_t packetsMIN  = (size + 1440 - 1) / 1440;
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

    void handle_cmd_info(struct sockaddr_in &si_other, struct SUDPBDv2_InfoRequest *request) {
        struct SUDPBDv2_InfoReply reply;

        char str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &si_other.sin_addr, str, INET_ADDRSTRLEN);

        printf("UDPBD_CMD_INFO from %s\n", str);

        // Reply header
        reply.hdr.cmd      = UDPBD_CMD_INFO_REPLY;
        reply.hdr.cmdid    = request->hdr.cmdid;
        reply.hdr.cmdpkt   = 1;
        // Reply info
        reply.sector_size  = _bd.get_sector_size();
        reply.sector_count = _bd.get_sector_count();

        // Send packet to ps2
        if (sendto(s, &reply, sizeof(reply), 0, (struct sockaddr*) &si_other, sizeof(si_other)) == -1) {
            throw runtime_error("sendto");
        }
    }

    void handle_cmd_read(struct sockaddr_in &si_other, struct SUDPBDv2_RWRequest *request) {
        struct SUDPBDv2_RDMA reply;

        printf("UDPBD_CMD_READ(cmdId=%d, startSector=%d, sectorCount=%d)\n", request->hdr.cmdid, request->sector_nr, request->sector_count);

        // Optimize RDMA block size for number of sectors
        set_block_shift_sectors(request->sector_count);

        // Reply header
        reply.hdr.cmd        = UDPBD_CMD_READ_RDMA;
        reply.hdr.cmdid      = request->hdr.cmdid;
        reply.hdr.cmdpkt     = 1;
        reply.bt.block_shift = _block_shift;

        uint32_t blocks_left = request->sector_count * _blocks_per_sector;

        _bd.seek(request->sector_nr);

        // Packet loop
        while (blocks_left > 0) {
            reply.bt.block_count = (blocks_left > _blocks_per_packet) ? _blocks_per_packet : blocks_left;
            blocks_left -= reply.bt.block_count;

            // read data from file
            _bd.read(reply.data, reply.bt.block_count * _block_size);

            // Send packet to ps2
            if (sendto(s, &reply, sizeof(struct SUDPBDv2_Header) + 4 + (reply.bt.block_count * _block_size), 0, (struct sockaddr*) &si_other, sizeof(si_other)) == -1) {
                throw runtime_error("sendto");
            }
            reply.hdr.cmdpkt++;
        }
    }

    void handle_cmd_write(struct sockaddr_in &si_other, struct SUDPBDv2_RWRequest *request) {
        printf("UDPBD_CMD_WRITE(cmdId=%d, startSector=%d, sectorCount=%d)\n", request->hdr.cmdid, request->sector_nr, request->sector_count);

        _bd.seek(request->sector_nr);
        _write_size_left = request->sector_count * 512;
    }

    void handle_cmd_write_rdma(struct sockaddr_in &si_other, struct SUDPBDv2_RDMA *request) {
        size_t size = request->bt.block_count * (1 << (request->bt.block_shift + 2));
        //printf("UDPBD_CMD_WRITE_RDMA(cmdId=%d, BS=%d, BC=%d, size=%ld)\n", request->hdr.cmdid, request->bt.block_shift, request->bt.block_count, size);

        _bd.write(request->data, size);   
        _write_size_left -= size;
        if(_write_size_left == 0) {
            struct SUDPBDv2_WriteDone reply;

            // Reply header
            reply.hdr.cmd      = UDPBD_CMD_WRITE_DONE;
            reply.hdr.cmdid    = request->hdr.cmdid;
            reply.hdr.cmdpkt   = request->hdr.cmdid + 1;
            reply.result       = 0; 

            // Send packet to ps2
            if (sendto(s, &reply, sizeof(reply), 0, (struct sockaddr*) &si_other, sizeof(si_other)) == -1) {
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

int main(int argc, char * argv[])
{
    if (argc < 2) {
        printf("Usage:\n");
        printf("  %s <file>\n", argv[0]);
        return -1;
    }

    try {
        class CBlockDevice bd(argv[1]);
        class CUDPBDServer srv(bd);
        srv.run();
    } catch (exception& e) {
        cout<<e.what()<<'\n';
        return -2;
    }

    return 0;
}
